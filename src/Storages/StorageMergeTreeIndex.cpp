
#include "Common/escapeForFileName.h"
#include "Storages/MergeTree/MergeTreeData.h"
#include <Storages/StorageMergeTreeIndex.h>
#include <Columns/ColumnTuple.h>
#include <Columns/ColumnsNumber.h>
#include <Columns/ColumnNullable.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/NestedUtils.h>
#include <Parsers/ASTSelectQuery.h>
#include <Storages/ColumnsDescription.h>
#include <Storages/MergeTree/LoadedMergeTreeDataPartInfoForReader.h>
#include <Storages/MergeTree/MergeTreeDataPartCompact.h>
#include <Storages/MergeTree/MergeTreeMarksLoader.h>
#include <Storages/VirtualColumnUtils.h>
#include <Access/Common/AccessFlags.h>
#include <Common/HashTable/HashSet.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int NO_SUCH_COLUMN_IN_TABLE;
    extern const int NOT_IMPLEMENTED;
}

class MergeTreeIndexSource : public ISource, WithContext
{
public:
    MergeTreeIndexSource(
        Block header_,
        Block index_header_,
        MergeTreeData::DataPartsVector data_parts_,
        ContextPtr context_,
        bool with_marks_)
        : ISource(header_)
        , WithContext(std::move(context_))
        , header(std::move(header_))
        , index_header(std::move(index_header_))
        , data_parts(std::move(data_parts_))
        , with_marks(with_marks_)
    {
    }

    String getName() const override { return "MergeTreeIndex"; }

protected:
    Chunk generate() override
    {
        if (part_index >= data_parts.size())
            return {};

        const auto & part = data_parts[part_index];
        const auto & index_granularity = part->index_granularity;

        std::shared_ptr<MergeTreeMarksLoader> marks_loader;
        if (with_marks && isCompactPart(part))
            marks_loader = createMarksLoader(part, MergeTreeDataPartCompact::DATA_FILE_NAME, part->getColumns().size());

        size_t num_columns = header.columns();
        size_t num_rows = index_granularity.getMarksCount();

        const auto & part_name_column = StorageMergeTreeIndex::part_name_column;
        const auto & mark_number_column = StorageMergeTreeIndex::mark_number_column;
        const auto & rows_in_granule_column = StorageMergeTreeIndex::rows_in_granule_column;

        Columns result_columns(num_columns);
        for (size_t pos = 0; pos < num_columns; ++pos)
        {
            const auto & column_name = header.getByPosition(pos).name;
            const auto & column_type = header.getByPosition(pos).type;

            if (index_header.has(column_name))
            {
                size_t index_position = index_header.getPositionByName(column_name);
                result_columns[pos] = part->index[index_position];
            }
            else if (column_name == part_name_column.name)
            {
                auto column = column_type->createColumnConst(num_rows, part->name);
                result_columns[pos] = column->convertToFullColumnIfConst();
            }
            else if (column_name == mark_number_column.name)
            {
                auto column = column_type->createColumn();
                auto & data = assert_cast<ColumnUInt64 &>(*column).getData();

                data.resize(num_rows);
                std::iota(data.begin(), data.end(), 0);

                result_columns[pos] = std::move(column);
            }
            else if (column_name == rows_in_granule_column.name)
            {
                auto column = column_type->createColumn();
                auto & data = assert_cast<ColumnUInt64 &>(*column).getData();

                data.resize(num_rows);
                for (size_t i = 0; i < num_rows; ++i)
                    data[i] = index_granularity.getMarkRows(i);

                result_columns[pos] = std::move(column);
            }
            else if (auto [first, second] = Nested::splitName(column_name, true); with_marks && second == "mark")
            {
                result_columns[pos] = fillMarks(part, marks_loader, *column_type, first);
            }
            else
            {
                throw Exception(ErrorCodes::NO_SUCH_COLUMN_IN_TABLE, "No such column {}", column_name);
            }
        }

        ++part_index;
        return Chunk(std::move(result_columns), num_rows);
    }

private:
    std::shared_ptr<MergeTreeMarksLoader> createMarksLoader(const MergeTreeDataPartPtr & part, const String & prefix_name, size_t num_columns)
    {
        auto info_for_read = std::make_shared<LoadedMergeTreeDataPartInfoForReader>(part, std::make_shared<AlterConversions>());
        auto local_context = getContext();

        return std::make_shared<MergeTreeMarksLoader>(
            info_for_read,
            local_context->getMarkCache().get(),
            info_for_read->getIndexGranularityInfo().getMarksFilePath(prefix_name),
            info_for_read->getMarksCount(),
            info_for_read->getIndexGranularityInfo(),
            /*save_marks_in_cache=*/ false,
            local_context->getReadSettings(),
            /*load_marks_threadpool=*/ nullptr,
            num_columns);
    }

    ColumnPtr fillMarks(
        MergeTreeDataPartPtr part,
        std::shared_ptr<MergeTreeMarksLoader> marks_loader,
        const IDataType & data_type,
        const String & column_name)
    {
        size_t col_idx = 0;
        bool has_marks_in_part = false;
        size_t num_rows = part->index_granularity.getMarksCount();

        if (isWidePart(part))
        {
            if (auto stream_name = part->getStreamNameOrHash(column_name, part->checksums))
            {
                col_idx = 0;
                has_marks_in_part = true;
                marks_loader = createMarksLoader(part, *stream_name, /*num_columns=*/ 1);
            }
        }
        else if (isCompactPart(part))
        {
            auto unescaped_name = unescapeForFileName(column_name);
            if (auto col_idx_opt = part->getColumnPosition(unescaped_name))
            {
                col_idx = *col_idx_opt;
                has_marks_in_part = true;
            }
        }
        else
        {
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Parts with type {} are not supported", part->getTypeName());
        }

        if (!has_marks_in_part)
        {
            auto column = data_type.createColumnConstWithDefaultValue(num_rows);
            return column->convertToFullColumnIfConst();
        }

        auto compressed = ColumnUInt64::create(num_rows);
        auto uncompressed = ColumnUInt64::create(num_rows);

        auto & compressed_data = compressed->getData();
        auto & uncompressed_data = uncompressed->getData();

        for (size_t i = 0; i < num_rows; ++i)
        {
            auto mark = marks_loader->getMark(i, col_idx);

            compressed_data[i] = mark.offset_in_compressed_file;
            uncompressed_data[i] = mark.offset_in_decompressed_block;
        }

        auto compressed_nullable = ColumnNullable::create(std::move(compressed), ColumnUInt8::create(num_rows, 0));
        auto uncompressed_nullable = ColumnNullable::create(std::move(uncompressed), ColumnUInt8::create(num_rows, 0));

        return ColumnTuple::create(Columns{std::move(compressed_nullable), std::move(uncompressed_nullable)});
    }

    Block header;
    Block index_header;
    MergeTreeData::DataPartsVector data_parts;
    bool with_marks;

    size_t part_index = 0;
};

const ColumnWithTypeAndName StorageMergeTreeIndex::part_name_column{std::make_shared<DataTypeString>(), "part_name"};
const ColumnWithTypeAndName StorageMergeTreeIndex::mark_number_column{std::make_shared<DataTypeUInt64>(), "mark_number"};
const ColumnWithTypeAndName StorageMergeTreeIndex::rows_in_granule_column{std::make_shared<DataTypeUInt64>(), "rows_in_granule"};
const Block StorageMergeTreeIndex::virtuals_sample_block{part_name_column, mark_number_column, rows_in_granule_column};

StorageMergeTreeIndex::StorageMergeTreeIndex(
    const StorageID & table_id_,
    const StoragePtr & source_table_,
    const ColumnsDescription & columns,
    bool with_marks_)
    : IStorage(table_id_)
    , source_table(source_table_)
    , with_marks(with_marks_)
    , log(&Poco::Logger::get("StorageMergeTreeIndex"))
{
    const auto * merge_tree = dynamic_cast<const MergeTreeData *>(source_table.get());
    if (!merge_tree)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Storage MergeTreeIndex expected MergeTree table, got: {}", source_table->getName());

    data_parts = merge_tree->getDataPartsVectorForInternalUsage();
    key_sample_block = merge_tree->getInMemoryMetadataPtr()->getPrimaryKey().sample_block;

    StorageInMemoryMetadata storage_metadata;
    storage_metadata.setColumns(columns);
    setInMemoryMetadata(storage_metadata);
}

Pipe StorageMergeTreeIndex::read(
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & query_info,
    ContextPtr context,
    QueryProcessingStage::Enum,
    size_t /*max_block_size*/,
    size_t /*num_streams*/)
{
    const auto & storage_columns = source_table->getInMemoryMetadataPtr()->getColumns();
    Names columns_from_storage;

    for (const auto & column_name : column_names)
    {
        if (storage_columns.hasColumnOrSubcolumn(GetColumnsOptions::All, column_name))
        {
            columns_from_storage.push_back(column_name);
            continue;
        }

        if (with_marks)
        {
            auto [first, second] = Nested::splitName(column_name, true);
            auto unescaped_name = unescapeForFileName(first);

            if (second == "mark" && storage_columns.hasColumnOrSubcolumn(GetColumnsOptions::All, unescapeForFileName(unescaped_name)))
            {
                columns_from_storage.push_back(unescaped_name);
                continue;
            }
        }
    }

    context->checkAccess(AccessType::SELECT, source_table->getStorageID(), columns_from_storage);

    auto header = storage_snapshot->getSampleBlockForColumns(column_names);
    auto filtered_parts = getFilteredDataParts(query_info, context);

    LOG_DEBUG(log, "Reading index{}from {} parts of table {}",
        with_marks ? " with marks " : " ",
        filtered_parts.size(),
        source_table->getStorageID().getNameForLogs());

    return Pipe(std::make_shared<MergeTreeIndexSource>(std::move(header), key_sample_block, std::move(filtered_parts), context, with_marks));
}

MergeTreeData::DataPartsVector StorageMergeTreeIndex::getFilteredDataParts(SelectQueryInfo & query_info, const ContextPtr & context) const
{
    const auto * select_query = query_info.query->as<ASTSelectQuery>();
    if (!select_query || !select_query->where())
        return data_parts;

    auto all_part_names = ColumnString::create();
    for (const auto & part : data_parts)
        all_part_names->insert(part->name);

    Block filtered_block {{std::move(all_part_names), std::make_shared<DataTypeString>(), part_name_column.name}};
    VirtualColumnUtils::filterBlockWithQuery(query_info.query, filtered_block, context);

    if (!filtered_block.rows())
        return {};

    auto part_names = filtered_block.getByPosition(0).column;
    const auto & part_names_str = assert_cast<const ColumnString &>(*part_names);

    HashSet<StringRef> part_names_set;
    for (size_t i = 0; i < part_names_str.size(); ++i)
        part_names_set.insert(part_names_str.getDataAt(i));

    MergeTreeData::DataPartsVector filtered_parts;
    for (const auto & part : data_parts)
        if (part_names_set.has(part->name))
            filtered_parts.push_back(part);

    return filtered_parts;
}

}
