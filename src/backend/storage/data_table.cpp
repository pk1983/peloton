//===----------------------------------------------------------------------===//
//
//                         PelotonDB
//
// data_table.cpp
//
// Identification: src/backend/storage/data_table.cpp
//
// Copyright (c) 2015, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <mutex>
#include <utility>

#include "backend/storage/data_table.h"
#include "backend/storage/database.h"

#include "backend/common/exception.h"
#include "backend/index/index.h"
#include "backend/common/logger.h"

namespace peloton {
namespace storage {

bool ContainsVisibleEntry(std::vector<ItemPointer>& locations,
                          const concurrency::Transaction* transaction);

/**
 * Check if the locations contains at least one visible entry to the transaction
 */
bool ContainsVisibleEntry(std::vector<ItemPointer>& locations,
                          const concurrency::Transaction* transaction) {
  auto &manager = catalog::Manager::GetInstance();

  for (auto loc : locations) {

    oid_t tile_group_id = loc.block;
    oid_t tuple_offset = loc.offset;

    auto tile_group = manager.GetTileGroup(tile_group_id);
    auto header = tile_group->GetHeader();

    auto transaction_id = transaction->GetTransactionId();
    auto last_commit_id = transaction->GetLastCommitId();
    bool visible = header->IsVisible(tuple_offset, transaction_id,
                                     last_commit_id);

    if (visible)
      return true;
  }

  return false;
}

DataTable::DataTable(catalog::Schema *schema, AbstractBackend *backend,
                     std::string table_name, oid_t table_oid,
                     size_t tuples_per_tilegroup,
                     bool own_schema)
    : AbstractTable(table_oid, table_name, schema, own_schema),
      backend(backend),
      tuples_per_tilegroup(tuples_per_tilegroup) {
  // Create a tile group.
  AddDefaultTileGroup();
}

DataTable::~DataTable() {
  // clean up tile groups
  oid_t tile_group_count = GetTileGroupCount();
  for (oid_t tile_group_itr = 0; tile_group_itr < tile_group_count;
      tile_group_itr++) {
    auto tile_group = GetTileGroup(tile_group_itr);
    delete tile_group;
  }

  // clean up indices
  for (auto index : indexes) {
    delete index;
  }

  // clean up foreign keys
  for (auto foreign_key : foreign_keys) {
    delete foreign_key;
  }

  // table owns its backend
  // TODO: Should we really be doing this here?
  delete backend;

  // AbstractTable cleans up the schema
}

//===--------------------------------------------------------------------===//
// TUPLE HELPER OPERATIONS
//===--------------------------------------------------------------------===//

bool DataTable::CheckNulls(const storage::Tuple *tuple) const {
  assert(schema->GetColumnCount() == tuple->GetColumnCount());

  oid_t column_count = schema->GetColumnCount();
  for (int column_itr = column_count - 1; column_itr >= 0; --column_itr) {
    if (tuple->IsNull(column_itr) && schema->AllowNull(column_itr) == false) {
      LOG_TRACE(
          "%d th attribute in the tuple was NULL. It is non-nullable " "attribute.",
          column_itr);
      return false;
    }
  }

  return true;
}

bool DataTable::CheckConstraints(const storage::Tuple *tuple) const {
  // First, check NULL constraints
  if (CheckNulls(tuple) == false) {
    throw ConstraintException(
        "Not NULL constraint violated : " + tuple->GetInfo());
    return false;
  }

  return true;
}

ItemPointer DataTable::GetTupleSlot(const concurrency::Transaction *transaction,
                                    const storage::Tuple *tuple) {
  assert(tuple);

  if (CheckConstraints(tuple) == false)
    return INVALID_ITEMPOINTER;

  TileGroup *tile_group = nullptr;
  oid_t tuple_slot = INVALID_OID;
  oid_t tile_group_offset = INVALID_OID;
  auto transaction_id = transaction->GetTransactionId();

  while (tuple_slot == INVALID_OID) {
    // First, figure out last tile group
    {
      std::lock_guard<std::mutex> lock(table_mutex);
      assert(GetTileGroupCount() > 0);
      tile_group_offset = GetTileGroupCount() - 1;
      LOG_TRACE("Tile group offset :: %d \n", tile_group_offset);
    }

    // Then, try to grab a slot in the tile group header
    tile_group = GetTileGroup(tile_group_offset);
    tuple_slot = tile_group->InsertTuple(transaction_id, tuple);
    if (tuple_slot == INVALID_OID) {
      // XXX Should we put this in a critical section?
      AddDefaultTileGroup();
    }
  }

  LOG_INFO("tile group offset: %u, tile group id: %u, address: %p",
           tile_group_offset, tile_group->GetTileGroupId(), tile_group);

  // Set tuple location
  ItemPointer location(tile_group->GetTileGroupId(), tuple_slot);

  return location;
}

//===--------------------------------------------------------------------===//
// INSERT
//===--------------------------------------------------------------------===//

ItemPointer DataTable::InsertTuple(const concurrency::Transaction *transaction,
                                   const storage::Tuple *tuple) {
  // First, do integrity checks and claim a slot
  ItemPointer location = GetTupleSlot(transaction, tuple);
  if (location.block == INVALID_OID){
    LOG_WARN("Failed to get tuple slot.");
    return INVALID_ITEMPOINTER;
  }

  LOG_INFO("Location: %d, %d", location.block, location.offset);

  // Index checks and updates
  if (InsertInIndexes(transaction, tuple, location) == false) {
    LOG_WARN("Index constraint violated\n");
    return INVALID_ITEMPOINTER;
  }

  // Increase the table's number of tuples by 1
  IncreaseNumberOfTuplesBy(1);
  // Increase the indexes' number of tuples by 1 as well
  for (auto index : indexes)
    index->IncreaseNumberOfTuplesBy(1);

  return location;
}

/**
 * @brief Insert a tuple into all indexes. If index is primary/unique,
 * check visibility of existing
 * index entries.
 * @warning This still doesn't guarantee serializability.
 *
 * @returns True on success, false if a visible entry exists (in case of primary/unique).
 */
bool DataTable::InsertInIndexes(const concurrency::Transaction *transaction,
                                const storage::Tuple *tuple,
                                ItemPointer location) {

  int index_count = GetIndexCount();

  // (A) Check existence for primary/unique indexes
  // FIXME Since this is NOT protected by a lock, concurrent insert may happen.
  for (int index_itr = index_count - 1; index_itr >= 0; --index_itr) {
    auto index = GetIndex(index_itr);
    auto index_schema = index->GetKeySchema();
    auto indexed_columns = index_schema->GetIndexedColumns();
    std::unique_ptr<storage::Tuple> key(new storage::Tuple(index_schema, true));
    key->SetFromTuple(tuple, indexed_columns);

    switch (index->GetIndexType()) {
      case INDEX_CONSTRAINT_TYPE_PRIMARY_KEY:
      case INDEX_CONSTRAINT_TYPE_UNIQUE: {
        auto locations = index->Scan(key.get());
        auto exist_visible = ContainsVisibleEntry(locations, transaction);
        if (exist_visible) {
          LOG_WARN("A visible index entry exists.");
          return false;
        }
      }
        break;

      case INDEX_CONSTRAINT_TYPE_DEFAULT:
      default:
        break;
    }
    LOG_INFO("Index constraint check on %s passed.", index->GetName().c_str());
  }

  // (B) Insert into index
  for (int index_itr = index_count - 1; index_itr >= 0; --index_itr) {
    auto index = GetIndex(index_itr);
    auto index_schema = index->GetKeySchema();
    auto indexed_columns = index_schema->GetIndexedColumns();
    std::unique_ptr<storage::Tuple> key(new storage::Tuple(index_schema, true));
    key->SetFromTuple(tuple, indexed_columns);

    auto status = index->InsertEntry(key.get(), location);
    (void) status;
    assert(status);
  }

  return true;
}

//===--------------------------------------------------------------------===//
// DELETE
//===--------------------------------------------------------------------===//

/**
 * @brief Try to delete a tuple from the table.
 * It may fail because the tuple has been latched or conflict with a future delete.
 *
 * @param transaction_id  The current transaction Id.
 * @param location        ItemPointer of the tuple to delete.
 * NB: location.block should be the tile_group's \b ID, not \b offset.
 * @return True on success, false on failure.
 */
bool DataTable::DeleteTuple(const concurrency::Transaction *transaction,
                            ItemPointer location) {
  oid_t tile_group_id = location.block;
  oid_t tuple_id = location.offset;

  auto tile_group = GetTileGroupById(tile_group_id);
  txn_id_t transaction_id = transaction->GetTransactionId();
  cid_t last_cid = transaction->GetLastCommitId();

  // Delete slot in underlying tile group
  auto status = tile_group->DeleteTuple(transaction_id, tuple_id, last_cid);
  if (status == false) {
    LOG_WARN("Failed to delete tuple from the tile group : %u , Txn_id : %lu ",
             tile_group_id, transaction_id);
    return false;
  }

  LOG_TRACE("Deleted location :: block = %u offset = %u \n", location.block,
            location.offset);
  // Decrease the table's number of tuples by 1
  DecreaseNumberOfTuplesBy(1);

  return true;
}

//===--------------------------------------------------------------------===//
// UPDATE
//===--------------------------------------------------------------------===//
/**
 * @return Location of the newly inserted (updated) tuple
 */
ItemPointer DataTable::UpdateTuple(const concurrency::Transaction *transaction,
                                   const storage::Tuple *tuple) {
  // Do integrity checks and claim a slot
  ItemPointer location = GetTupleSlot(transaction, tuple);
  if (location.block == INVALID_OID)
    return INVALID_ITEMPOINTER;

  bool status = false;
  // 1) Try as if it's a same-key update
  status = UpdateInIndexes(tuple, location);

  // 2) If 1) fails, try again as an Insert
  if (false == status) {
    status = InsertInIndexes(transaction, tuple, location);
  }

  // 3) If still fails, then it is a real failure
  if (false == status) {
    location = INVALID_ITEMPOINTER;
  }

  return location;
}

bool DataTable::UpdateInIndexes(const storage::Tuple *tuple,
                                const ItemPointer location) {
  for (auto index : indexes) {
    auto index_schema = index->GetKeySchema();
    auto indexed_columns = index_schema->GetIndexedColumns();

    storage::Tuple *key = new storage::Tuple(index_schema, true);
    key->SetFromTuple(tuple, indexed_columns);

    if (index->UpdateEntry(key, location) == false) {
      LOG_TRACE("Same-key update index failed \n");
      delete key;
      return false;
    }

    delete key;
  }

  return true;
}

//===--------------------------------------------------------------------===//
// STATS
//===--------------------------------------------------------------------===//

/**
 * @brief Increase the number of tuples in this table
 * @param amount amount to increase
 */
void DataTable::IncreaseNumberOfTuplesBy(const float amount) {
  number_of_tuples += amount;
  dirty = true;
}

/**
 * @brief Decrease the number of tuples in this table
 * @param amount amount to decrease
 */
void DataTable::DecreaseNumberOfTuplesBy(const float amount) {
  number_of_tuples -= amount;
  dirty = true;
}

/**
 * @brief Set the number of tuples in this table
 * @param num_tuples number of tuples
 */
void DataTable::SetNumberOfTuples(const float num_tuples) {
  number_of_tuples = num_tuples;
  dirty = true;
}

/**
 * @brief Get the number of tuples in this table
 * @return number of tuples
 */
float DataTable::GetNumberOfTuples() const {
  return number_of_tuples;
}

/**
 * @brief return dirty flag
 * @return dirty flag
 */
bool DataTable::IsDirty() const {
  return dirty;
}

/**
 * @brief Reset dirty flag
 */
void DataTable::ResetDirty() {
  dirty = false;
}

//===--------------------------------------------------------------------===//
// TILE GROUP
//===--------------------------------------------------------------------===//

oid_t DataTable::AddDefaultTileGroup() {
  oid_t tile_group_id = INVALID_OID;

  std::vector<catalog::Schema> schemas;
  column_map_type column_map;

  tile_group_id = catalog::Manager::GetInstance().GetNextOid();
  schemas.push_back(*schema);

  // default column map
  auto col_count = schema->GetColumnCount();
  for (oid_t col_itr = 0; col_itr < col_count; col_itr++) {
    column_map[col_itr] = std::make_pair(0, col_itr);
  }

  TileGroup *tile_group = TileGroupFactory::GetTileGroup(database_oid,
                                                         table_oid,
                                                         tile_group_id, this,
                                                         backend, schemas,
                                                         column_map,
                                                         tuples_per_tilegroup);

  LOG_TRACE("Trying to add a tile group \n");
  {
    std::lock_guard<std::mutex> lock(table_mutex);

    // Check if we actually need to allocate a tile group

    // (A) no tile groups in table
    if (tile_groups.empty()) {
      LOG_TRACE("Added first tile group \n");
      tile_groups.push_back(tile_group->GetTileGroupId());
      // add tile group metadata in locator
      catalog::Manager::GetInstance().SetTileGroup(tile_group_id, tile_group);
      LOG_TRACE("Recording tile group : %d \n", tile_group_id);
      return tile_group_id;
    }

    // (B) no slots in last tile group in table
    auto last_tile_group_offset = GetTileGroupCount() - 1;
    auto last_tile_group = GetTileGroup(last_tile_group_offset);

    oid_t active_tuple_count = last_tile_group->GetNextTupleSlot();
    oid_t allocated_tuple_count = last_tile_group->GetAllocatedTupleCount();
    if (active_tuple_count < allocated_tuple_count) {
      LOG_TRACE("Slot exists in last tile group :: %d %d \n", active_tuple_count,
               allocated_tuple_count);
      delete tile_group;
      return INVALID_OID;
    }

    LOG_TRACE("Added a tile group \n");
    tile_groups.push_back(tile_group->GetTileGroupId());

    // add tile group metadata in locator
    catalog::Manager::GetInstance().SetTileGroup(tile_group_id, tile_group);
    LOG_TRACE("Recording tile group : %d \n", tile_group_id);
  }

  return tile_group_id;
}

void DataTable::AddTileGroup(TileGroup *tile_group) {
  {
    std::lock_guard<std::mutex> lock(table_mutex);

    tile_groups.push_back(tile_group->GetTileGroupId());
    oid_t tile_group_id = tile_group->GetTileGroupId();

    // add tile group metadata in locator
    catalog::Manager::GetInstance().SetTileGroup(tile_group_id, tile_group);
    LOG_TRACE("Recording tile group : %d \n", tile_group_id);
  }
}

size_t DataTable::GetTileGroupCount() const {
  size_t size = tile_groups.size();
  return size;
}

TileGroup *DataTable::GetTileGroup(oid_t tile_group_offset) const {
  assert(tile_group_offset < GetTileGroupCount());
  auto tile_group_id = tile_groups[tile_group_offset];
  return GetTileGroupById(tile_group_id);
}

TileGroup *DataTable::GetTileGroupById(oid_t tile_group_id) const {
  auto &manager = catalog::Manager::GetInstance();
  storage::TileGroup *tile_group = manager.GetTileGroup(tile_group_id);
  assert(tile_group);
  return tile_group;
}

std::ostream &operator<<(std::ostream &os, const DataTable &table) {
  os << "=====================================================\n";
  os << "TABLE :\n";

  oid_t tile_group_count = table.GetTileGroupCount();
  os << "Tile Group Count : " << tile_group_count << "\n";

  oid_t tuple_count = 0;
  for (oid_t tile_group_itr = 0; tile_group_itr < tile_group_count;
      tile_group_itr++) {
    auto tile_group = table.GetTileGroup(tile_group_itr);
    auto tile_tuple_count = tile_group->GetNextTupleSlot();

    os << "Tile Group Id  : " << tile_group_itr << " Tuple Count : "
       << tile_tuple_count << "\n";
    os << (*tile_group);

    tuple_count += tile_tuple_count;
  }

  os << "Table Tuple Count :: " << tuple_count << "\n";

  os << "=====================================================\n";

  return os;
}

//===--------------------------------------------------------------------===//
// INDEX
//===--------------------------------------------------------------------===//

void DataTable::AddIndex(index::Index *index) {
  {
    std::lock_guard<std::mutex> lock(table_mutex);
    indexes.push_back(index);
  }

  // Update index stats
  auto index_type = index->GetIndexType();
  if (index_type == INDEX_CONSTRAINT_TYPE_PRIMARY_KEY) {
    has_primary_key = true;
  } else if (index_type == INDEX_CONSTRAINT_TYPE_UNIQUE) {
    unique_constraint_count++;
  }
}

index::Index *DataTable::GetIndexWithOid(const oid_t index_oid) const {
  for (auto index : indexes)
    if (index->GetOid() == index_oid)
      return index;

  return nullptr;
}

void DataTable::DropIndexWithOid(const oid_t index_id) {
  {
    std::lock_guard<std::mutex> lock(table_mutex);

    oid_t index_offset = 0;
    for (auto index : indexes) {
      if (index->GetOid() == index_id)
        break;
      index_offset++;
    }
    assert(index_offset < indexes.size());

    // Drop the index
    indexes.erase(indexes.begin() + index_offset);
  }
}

index::Index *DataTable::GetIndex(const oid_t index_offset) const {
  assert(index_offset < indexes.size());
  auto index = indexes.at(index_offset);
  return index;
}

oid_t DataTable::GetIndexCount() const {
  return indexes.size();
}

//===--------------------------------------------------------------------===//
// FOREIGN KEYS
//===--------------------------------------------------------------------===//

void DataTable::AddForeignKey(catalog::ForeignKey *key) {
  {
    std::lock_guard<std::mutex> lock(table_mutex);
    catalog::Schema *schema = this->GetSchema();
    catalog::Constraint constraint(CONSTRAINT_TYPE_FOREIGN,
                                   key->GetConstraintName());
    constraint.SetForeignKeyListOffset(GetForeignKeyCount());
    for (auto fk_column : key->GetFKColumnNames()) {
      schema->AddConstraint(fk_column, constraint);
    }
    // TODO :: We need this one..
    catalog::ForeignKey *fk = new catalog::ForeignKey(*key);
    foreign_keys.push_back(fk);
  }
}

catalog::ForeignKey *DataTable::GetForeignKey(const oid_t key_offset) const {
  catalog::ForeignKey *key = nullptr;
  key = foreign_keys.at(key_offset);
  return key;
}

void DataTable::DropForeignKey(const oid_t key_offset) {
  {
    std::lock_guard<std::mutex> lock(table_mutex);
    assert(key_offset < foreign_keys.size());
    foreign_keys.erase(foreign_keys.begin() + key_offset);
  }
}

oid_t DataTable::GetForeignKeyCount() const {
  return foreign_keys.size();
}

// Get the schema for the new transformed tile group
std::vector<catalog::Schema> TransformTileGroupSchema(
    storage::TileGroup *tile_group, const column_map_type &column_map) {
  std::vector<catalog::Schema> new_schema;
  oid_t orig_tile_offset, orig_tile_column_offset;
  oid_t new_tile_offset, new_tile_column_offset;

  // First, get info from the original tile group's schema
  std::map<oid_t, std::map<oid_t, catalog::Column>> schemas;
  auto orig_schemas = tile_group->GetTileSchemas();
  for (auto column_map_entry : column_map) {
    new_tile_offset = column_map_entry.second.first;
    new_tile_column_offset = column_map_entry.second.second;
    oid_t column_offset = column_map_entry.first;

    tile_group->LocateTileAndColumn(column_offset, orig_tile_offset,
                                    orig_tile_column_offset);

    // Get the column info from original schema
    auto orig_schema = orig_schemas[orig_tile_offset];
    auto column_info = orig_schema.GetColumn(orig_tile_column_offset);
    schemas[new_tile_offset][new_tile_column_offset] = column_info;
  }

  // Then, build the new schema
  for (auto schemas_tile_entry : schemas) {
    std::vector<catalog::Column> columns;
    for (auto schemas_column_entry : schemas_tile_entry.second)
      columns.push_back(schemas_column_entry.second);

    catalog::Schema tile_schema(columns);
    new_schema.push_back(tile_schema);
  }

  return new_schema;
}

// Set the transformed tile group column-at-a-time
void SetTransformedTileGroup(storage::TileGroup *orig_tile_group,
                             storage::TileGroup *new_tile_group) {
  // Check the schema of the two tile groups
  auto new_column_map = new_tile_group->GetColumnMap();
  auto orig_column_map = orig_tile_group->GetColumnMap();
  assert(new_column_map.size() == orig_column_map.size());

  oid_t orig_tile_offset, orig_tile_column_offset;
  oid_t new_tile_offset, new_tile_column_offset;

  auto column_count = new_column_map.size();
  auto tuple_count = orig_tile_group->GetAllocatedTupleCount();
  // Go over each column copying onto the new tile group
  for (oid_t column_itr = 0; column_itr < column_count; column_itr++) {
    // Locate the original base tile and tile column offset
    orig_tile_group->LocateTileAndColumn(column_itr, orig_tile_offset,
                                         orig_tile_column_offset);

    new_tile_group->LocateTileAndColumn(column_itr, new_tile_offset,
                                        new_tile_column_offset);

    auto orig_tile = orig_tile_group->GetTile(orig_tile_offset);
    auto new_tile = new_tile_group->GetTile(new_tile_offset);

    // Copy the column over to the new tile group
    for (oid_t tuple_itr = 0; tuple_itr < tuple_count; tuple_itr++) {
      auto val = orig_tile->GetValue(tuple_itr, orig_tile_column_offset);
      new_tile->SetValue(val, tuple_itr, new_tile_column_offset);
    }
  }

  // Finally, copy over the tile header
  auto header = orig_tile_group->GetHeader();
  auto new_header = new_tile_group->GetHeader();
  *new_header = *header;
}

storage::TileGroup *DataTable::TransformTileGroup(
    oid_t tile_group_id, const column_map_type &column_map, bool cleanup) {
  // First, check if the tile group is in this table
  {
    std::lock_guard<std::mutex> lock(table_mutex);

    auto found_itr = std::find(tile_groups.begin(), tile_groups.end(),
                               tile_group_id);

    if (found_itr == tile_groups.end()) {
      LOG_ERROR("Tile group not found in table : %u \n", tile_group_id);
      return nullptr;
    }
  }

  // Get orig tile group from catalog
  auto &catalog_manager = catalog::Manager::GetInstance();
  auto tile_group = catalog_manager.GetTileGroup(tile_group_id);

  // Get the schema for the new transformed tile group
  auto new_schema = TransformTileGroupSchema(tile_group, column_map);

  // Allocate space for the transformed tile group
  auto new_tile_group = TileGroupFactory::GetTileGroup(
      tile_group->GetDatabaseId(), tile_group->GetTableId(),
      tile_group->GetTileGroupId(), tile_group->GetAbstractTable(),
      tile_group->GetBackend(), new_schema, column_map,
      tile_group->GetAllocatedTupleCount());

  // Set the transformed tile group column-at-a-time
  SetTransformedTileGroup(tile_group, new_tile_group);

  // Set the location of the new tile group
  catalog_manager.SetTileGroup(tile_group_id, new_tile_group);

  // Clean up the orig tile group, if needed which is normally the case
  if (cleanup)
    delete tile_group;

  return new_tile_group;
}

}  // End storage namespace
}  // End peloton namespace
