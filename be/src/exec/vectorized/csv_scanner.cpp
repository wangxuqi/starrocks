// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#include "exec/vectorized/csv_scanner.h"

#include "column/column_helper.h"
#include "column/hash_set.h"
#include "env/env.h"
#include "gutil/strings/substitute.h"
#include "runtime/runtime_state.h"
#include "util/utf8_check.h"

namespace starrocks::vectorized {

/// CSVScanner::CSVReader
Status CSVScanner::CSVReader::next_record(Record* record) {
    if (_limit > 0 && _parsed_bytes > _limit) {
        return Status::EndOfFile("Reached limit");
    }
    char* d;
    size_t pos = 0;
    while ((d = _buff.find(_record_delimiter, pos)) == nullptr) {
        pos = _buff.available();
        _buff.compact();
        if (_buff.free_space() == 0) {
            RETURN_IF_ERROR(_expand_buffer());
        }
        RETURN_IF_ERROR(_fill_buffer());
    }
    size_t l = d - _buff.position();
    *record = Record(_buff.position(), l);
    _buff.skip(l + 1);
    //               ^^ skip record delimiter.
    _parsed_bytes += l + 1;
    return Status::OK();
}

Status CSVScanner::CSVReader::_fill_buffer() {
    SCOPED_RAW_TIMER(&_counter->file_read_ns);

    DCHECK(_buff.free_space() > 0);
    Slice s(_buff.limit(), _buff.free_space());
    Status st = _file->read(&s);
    // According to the specification of `Env::read`, when reached the end of
    // a file, the returned status will be OK instead of EOF, but here we check
    // EOF also for safety.
    if (st.is_end_of_file()) {
        s.size = 0;
    } else if (!st.ok()) {
        return st;
    }
    _buff.add_limit(s.size);
    auto n = _buff.available();
    if (s.size == 0 && n == 0) {
        // Has reached the end of file and the buffer is empty.
        return Status::EndOfFile(_file->filename());
    } else if (s.size == 0 && _buff.position()[n - 1] != _record_delimiter) {
        // Has reached the end of file but still no record delimiter found, which
        // is valid, according the RFC, add the record delimiter ourself.
        _buff.append(_record_delimiter);
    }
    return Status::OK();
}

Status CSVScanner::CSVReader::_expand_buffer() {
    if (UNLIKELY(_storage.size() >= kMaxBufferSize)) {
        return Status::InternalError("CSV line length exceed limit " + std::to_string(kMaxBufferSize));
    }
    size_t new_capacity = std::min(_storage.size() * 2, kMaxBufferSize);
    DCHECK_EQ(_storage.data(), _buff.position()) << "should compact buffer before expand";
    _storage.resize(new_capacity);
    Buffer new_buff(_storage.data(), _storage.size());
    new_buff.add_limit(_buff.available());
    DCHECK_EQ(_storage.data(), new_buff.position());
    DCHECK_EQ(_buff.available(), new_buff.available());
    _buff = new_buff;
    return Status::OK();
}

void CSVScanner::CSVReader::split_record(const Record& record, Fields* fields) const {
    const char* value = record.data;
    const char* ptr = record.data;
    const size_t size = record.size;
    for (size_t i = 0; i < size; ++i, ++ptr) {
        if (*ptr == _field_delimiter) {
            fields->emplace_back(value, ptr - value);
            value = ptr + 1;
        }
    }
    fields->emplace_back(value, ptr - value);
}

CSVScanner::CSVScanner(RuntimeState* state, RuntimeProfile* profile, const TBrokerScanRange& scan_range,
                       ScannerCounter* counter)
        : FileScanner(state, profile, scan_range.params, counter),
          _scan_range(scan_range),
          _record_delimiter(scan_range.params.row_delimiter),
          _field_delimiter(scan_range.params.column_separator) {}

Status CSVScanner::open() {
    RETURN_IF_ERROR(FileScanner::open());

    if (_scan_range.ranges.empty()) {
        return Status::OK();
    }

    const auto& first_range = _scan_range.ranges[0];
    if (!first_range.__isset.num_of_columns_from_file) {
        return Status::InternalError("'num_of_columns_from_file' not set");
    }

    for (const auto& rng : _scan_range.ranges) {
        if (rng.columns_from_path.size() != first_range.columns_from_path.size()) {
            return Status::InvalidArgument("path column count of range mismatch");
        }
        if (rng.num_of_columns_from_file != first_range.num_of_columns_from_file) {
            return Status::InvalidArgument("CSV column count of range mismatch");
        }
        if (rng.num_of_columns_from_file + rng.columns_from_path.size() != _src_slot_descriptors.size()) {
            return Status::InvalidArgument("slot descriptor and column count mismatch");
        }
    }

    _num_fields_in_csv = first_range.num_of_columns_from_file;

    for (int i = _num_fields_in_csv; i < _src_slot_descriptors.size(); i++) {
        if (_src_slot_descriptors[i]->type().type != TYPE_VARCHAR) {
            auto t = _src_slot_descriptors[i]->type();
            return Status::InvalidArgument("Incorrect path column type '" + t.debug_string() + "'");
        }
    }

    for (int i = 0; i < _num_fields_in_csv; i++) {
        auto slot = _src_slot_descriptors[i];
        if (slot == nullptr) {
            // This means the i-th field in CSV file should be ignored.
            continue;
        }
        // NOTE: Here we always create a nullable converter, even if |slot->is_nullable()| is false,
        // since |slot->is_nullable()| is false does not guarantee that no NULL in the CSV files.
        // This implies that the input column of |conv| must be a `NullableColumn`.
        //
        // For those columns defined as non-nullable, NULL records will be filtered out by the
        // `TabletSink`.
        ConverterPtr conv = csv::get_converter(slot->type(), true);
        if (conv == nullptr) {
            auto msg = strings::Substitute("Unsupported CSV type $0", slot->type().debug_string());
            return Status::InternalError(msg);
        }
        _converters.emplace_back(std::move(conv));
    }

    return Status::OK();
}

StatusOr<ChunkPtr> CSVScanner::get_next() {
    SCOPED_RAW_TIMER(&_counter->total_ns);

    ChunkPtr chunk;
    const int chunk_capacity = config::vector_chunk_size;
    auto src_chunk = _create_chunk(_src_slot_descriptors);
    src_chunk->reserve(chunk_capacity);

    do {
        if (_curr_reader == nullptr && ++_curr_file_index < _scan_range.ranges.size()) {
            std::shared_ptr<SequentialFile> file;
            const TBrokerRangeDesc& range_desc = _scan_range.ranges[_curr_file_index];
            Status st = create_sequential_file(range_desc, _scan_range.broker_addresses[0], _scan_range.params, &file);
            if (!st.ok()) {
                LOG(WARNING) << "Failed to create sequential files: " << st.to_string();
                return st;
            }

            _curr_reader = std::make_unique<CSVReader>(file, _record_delimiter, _field_delimiter);
            _curr_reader->set_counter(_counter);
            if (_scan_range.ranges[_curr_file_index].size > 0 &&
                _scan_range.ranges[_curr_file_index].format_type == TFileFormatType::FORMAT_CSV_PLAIN) {
                // Does not set limit for compressed file.
                _curr_reader->set_limit(_scan_range.ranges[_curr_file_index].size);
            }
            if (_scan_range.ranges[_curr_file_index].start_offset > 0) {
                // Skip the first record started from |start_offset|.
                file->skip(_scan_range.ranges[_curr_file_index].start_offset);
                CSVReader::Record dummy;
                RETURN_IF_ERROR(_curr_reader->next_record(&dummy));
            }
        } else if (_curr_reader == nullptr) {
            return Status::EndOfFile("CSVScanner");
        }

        src_chunk->set_num_rows(0);
        Status status = _parse_csv(src_chunk.get());
        if (status.is_end_of_file()) {
            _curr_reader = nullptr;
            DCHECK_EQ(0, src_chunk->num_rows());
        } else if (!status.ok()) {
            return status;
        }

        fill_columns_from_path(src_chunk, _num_fields_in_csv, _scan_range.ranges[_curr_file_index].columns_from_path,
                               src_chunk->num_rows());
        chunk = _materialize(src_chunk);
    } while ((chunk)->num_rows() == 0);
    return std::move(chunk);
}

Status CSVScanner::_parse_csv(Chunk* chunk) {
    const int capacity = config::vector_chunk_size;
    DCHECK_EQ(0, chunk->num_rows());
    Status status;
    CSVReader::Record record;
    CSVReader::Fields fields;

    int num_columns = chunk->num_columns();
    _column_raw_ptrs.resize(num_columns);
    for (int i = 0; i < num_columns; i++) {
        _column_raw_ptrs[i] = chunk->get_column_by_index(i).get();
    }

    csv::Converter::Options options{.invalid_field_as_null = !_strict_mode};

    for (size_t num_rows = chunk->num_rows(); num_rows < capacity; /**/) {
        status = _curr_reader->next_record(&record);
        if (status.is_end_of_file()) {
            break;
        } else if (!status.ok()) {
            return status;
        } else if (record.empty()) {
            // always skip blank lines.
            continue;
        }

        fields.clear();
        _curr_reader->split_record(record, &fields);

        if (fields.size() != _num_fields_in_csv) {
            std::stringstream error_msg;
            error_msg << "column count mismatch, expect=" << _num_fields_in_csv << " real=" << fields.size();
            if (_counter->num_rows_filtered++ < 50) {
                _report_error(record.to_string(), error_msg.str());
            }
            continue;
        }
        if (!validate_utf8(record.data, record.size)) {
            if (_counter->num_rows_filtered++ < 50) {
                _report_error(record.to_string(), "Invalid UTF-8 data");
            }
            continue;
        }

        SCOPED_RAW_TIMER(&_counter->fill_ns);
        bool has_error = false;
        for (int j = 0, k = 0; j < _num_fields_in_csv; j++) {
            if (_src_slot_descriptors[j] == nullptr) {
                continue;
            }
            const Slice& field = fields[j];
            options.type_desc = &(_src_slot_descriptors[j]->type());
            if (!_converters[k]->read_string(_column_raw_ptrs[k], field, options)) {
                chunk->set_num_rows(num_rows);
                if (_counter->num_rows_filtered++ < 50) {
                    _report_error(record.to_string(), "invalid value '" + field.to_string() + "'");
                }
                has_error = true;
                break;
            }
            k++;
        }
        num_rows += !has_error;
    }
    return chunk->num_rows() > 0 ? Status::OK() : Status::EndOfFile("");
}

ChunkPtr CSVScanner::_create_chunk(const std::vector<SlotDescriptor*>& slots) {
    SCOPED_RAW_TIMER(&_counter->init_chunk_ns);

    auto chunk = std::make_shared<Chunk>();
    for (int i = 0; i < _num_fields_in_csv; ++i) {
        if (slots[i] == nullptr) {
            continue;
        }
        // NOTE: Always create a nullable column, even if |slot->is_nullable()| is false.
        // See the comment in `CSVScanner::Open` for reference.
        auto column = ColumnHelper::create_column(slots[i]->type(), true);
        chunk->append_column(std::move(column), slots[i]->id());
    }
    return chunk;
}

// TODO(zhuming): move this function to `FileScanner` or `FileScanNode`
ChunkPtr CSVScanner::_materialize(ChunkPtr& src_chunk) {
    SCOPED_RAW_TIMER(&_counter->materialize_ns);

    if (src_chunk->num_rows() == 0) {
        return src_chunk;
    }

    ChunkPtr dest_chunk = std::make_shared<Chunk>();

    // CREATE ROUTINE LOAD routine_load_job_1
    // on table COLUMNS (k1,k2,k3=k1)
    // The column k3 and k1 will pointer to the same entity.
    // The k3 should be copied to avoid this case.
    // column_pointers is a hashset to check the repeatability.
    HashSet<uintptr_t> column_pointers;
    int ctx_index = 0;
    int src_rows = src_chunk->num_rows();
    for (const auto& slot : _dest_tuple_desc->slots()) {
        if (!slot->is_materialized()) {
            continue;
        }

        int dest_index = ctx_index++;
        auto dst_col = _dest_expr_ctx[dest_index]->evaluate(src_chunk.get());
        uintptr_t col_pointer = reinterpret_cast<uintptr_t>(dst_col.get());
        if (column_pointers.contains(col_pointer)) {
            dst_col = dst_col->clone();
        } else {
            column_pointers.emplace(col_pointer);
        }
        dst_col = ColumnHelper::unfold_const_column(slot->type(), src_rows, dst_col);
        dest_chunk->append_column(dst_col, slot->id());
    }
    return dest_chunk;
}

void CSVScanner::_report_error(const std::string& line, const std::string& err_msg) {
    _state->append_error_msg_to_file(line, err_msg);
}

} // namespace starrocks::vectorized
