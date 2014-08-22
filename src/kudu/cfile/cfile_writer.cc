// Copyright (c) 2012, Cloudera, inc

#include <boost/foreach.hpp>
#include <endian.h>
#include <glog/logging.h>
#include <string>
#include <utility>

#include "kudu/cfile/block_pointer.h"
#include "kudu/cfile/cfile_writer.h"
#include "kudu/cfile/index_block.h"
#include "kudu/cfile/index_btree.h"
#include "kudu/common/key_encoder.h"
#include "kudu/cfile/type_encodings.h"
#include "kudu/util/env.h"
#include "kudu/util/coding.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/hexdump.h"

using std::string;

DEFINE_int32(cfile_default_block_size, 256*1024, "The default block size to use in cfiles");

DEFINE_string(cfile_default_compression_codec, "none",
              "Default cfile block compression codec.");

namespace kudu { namespace cfile {

const char kMagicString[] = "kuducfil";

static const size_t kBlockSizeLimit = 16 * 1024 * 1024; // 16MB

static CompressionType GetDefaultCompressionCodec() {
  return GetCompressionCodecType(FLAGS_cfile_default_compression_codec);
}

////////////////////////////////////////////////////////////
// Options
////////////////////////////////////////////////////////////
WriterOptions::WriterOptions()
  : block_size(FLAGS_cfile_default_block_size),
    index_block_size(32*1024),
    block_restart_interval(16),
    write_posidx(false),
    write_validx(false),
    storage_attributes(ColumnStorageAttributes(AUTO_ENCODING, GetDefaultCompressionCodec())) {
}


////////////////////////////////////////////////////////////
// CFileWriter
////////////////////////////////////////////////////////////


CFileWriter::CFileWriter(const WriterOptions &options,
                         DataType type,
                         bool is_nullable,
                         shared_ptr<WritableFile> file)
  : file_(file),
    off_(0),
    value_count_(0),
    options_(options),
    is_nullable_(is_nullable),
    datatype_(type),
    typeinfo_(GetTypeInfo(type)),
    key_encoder_(GetKeyEncoder(type)),
    state_(kWriterInitialized) {
  EncodingType encoding = options_.storage_attributes.encoding();
  Status s = TypeEncodingInfo::Get(type, encoding, &type_encoding_info_);
  if (!s.ok()) {
    // TODO: we should somehow pass some contextual info about the
    // tablet here.
    WARN_NOT_OK(s, "Falling back to default encoding");
    encoding_type_ = TypeEncodingInfo::GetDefaultEncoding(type);
    s = TypeEncodingInfo::Get(type,
                              encoding_type_,
                              &type_encoding_info_);
    CHECK_OK(s);
  } else {
    encoding_type_ = encoding;
  }

  compression_ = options_.storage_attributes.compression();
  if (compression_ == DEFAULT_COMPRESSION) {
    compression_ = GetDefaultCompressionCodec();
  }

  if (options.write_posidx) {
    posidx_builder_.reset(new IndexTreeBuilder(&options_,
                                               this));
  }

  if (options.write_validx) {
    validx_builder_.reset(new IndexTreeBuilder(&options_,
                                               this));
  }
}

Status CFileWriter::Start() {
  CHECK(state_ == kWriterInitialized) <<
    "bad state for Start(): " << state_;

  if (compression_ != NO_COMPRESSION) {
    shared_ptr<CompressionCodec> compression_codec;
    RETURN_NOT_OK(
        GetCompressionCodec(compression_, &compression_codec));
    block_compressor_ .reset(new CompressedBlockBuilder(compression_codec, kBlockSizeLimit));
  }

  CFileHeaderPB header;
  header.set_major_version(kCFileMajorVersion);
  header.set_minor_version(kCFileMinorVersion);
  FlushMetadataToPB(header.mutable_metadata());

  uint32_t pb_size = header.ByteSize();

  faststring buf;
  // First the magic.
  buf.append(kMagicString);
  // Then Length-prefixed header.
  PutFixed32(&buf, pb_size);
  if (!pb_util::AppendToString(header, &buf)) {
    return Status::Corruption("unable to encode header");
  }

  RETURN_NOT_OK_PREPEND(file_->Append(Slice(buf)), "Couldn't write header");
  off_ += buf.size();

  BlockBuilder *bb;
  RETURN_NOT_OK(type_encoding_info_->CreateBlockBuilder(&bb, &options_) );
  data_block_.reset(bb);

  if (is_nullable_) {
    size_t nrows = ((options_.block_size + typeinfo_->size() - 1) / typeinfo_->size());
    null_bitmap_builder_.reset(new NullBitmapBuilder(nrows * 8));
  }

  state_ = kWriterWriting;

  return Status::OK();
}

Status CFileWriter::Finish() {
  CHECK(state_ == kWriterWriting) <<
    "Bad state for Finish(): " << state_;

  // Write out any pending values as the last data block.
  RETURN_NOT_OK(FinishCurDataBlock());

  state_ = kWriterFinished;

  // Start preparing the footer.
  CFileFooterPB footer;
  footer.set_data_type(datatype_);
  footer.set_is_type_nullable(is_nullable_);
  footer.set_encoding(options_.storage_attributes.encoding());
  footer.set_num_values(value_count_);
  footer.set_compression(compression_);

  // Write out any pending positional index blocks.
  if (options_.write_posidx) {
    BTreeInfoPB posidx_info;
    RETURN_NOT_OK_PREPEND(posidx_builder_->Finish(&posidx_info),
                          "Couldn't write positional index");
    footer.mutable_posidx_info()->CopyFrom(posidx_info);
  }

  if (options_.write_validx) {
    BTreeInfoPB validx_info;
    RETURN_NOT_OK_PREPEND(validx_builder_->Finish(&validx_info), "Couldn't write value index");
    footer.mutable_validx_info()->CopyFrom(validx_info);
  }

  // Flush metadata.
  FlushMetadataToPB(footer.mutable_metadata());

  faststring footer_str;
  if (!pb_util::SerializeToString(footer, &footer_str)) {
    return Status::Corruption("unable to serialize footer");
  }

  footer_str.append(kMagicString);
  PutFixed32(&footer_str, footer.GetCachedSize());

  RETURN_NOT_OK(file_->Append(footer_str));
  RETURN_NOT_OK(file_->Flush());

  return file_->Close();
}

void CFileWriter::AddMetadataPair(const Slice &key, const Slice &value) {
  CHECK_NE(state_, kWriterFinished);

  unflushed_metadata_.push_back(make_pair(key.ToString(), value.ToString()));
}

void CFileWriter::FlushMetadataToPB(RepeatedPtrField<FileMetadataPairPB> *field) {
  typedef pair<string, string> ss_pair;
  BOOST_FOREACH(const ss_pair &entry, unflushed_metadata_) {
    FileMetadataPairPB *pb = field->Add();
    pb->set_key(entry.first);
    pb->set_value(entry.second);
  }
  unflushed_metadata_.clear();
}

Status CFileWriter::AppendEntries(const void *entries, size_t count) {
  DCHECK(!is_nullable_);

  int rem = count;

  const uint8_t *ptr = reinterpret_cast<const uint8_t *>(entries);

  while (rem > 0) {
    int n = data_block_->Add(ptr, rem);
    DCHECK_GE(n, 0);

    ptr += typeinfo_->size() * n;
    rem -= n;
    value_count_ += n;

    size_t est_size = data_block_->EstimateEncodedSize();
    if (est_size > options_.block_size) {
      RETURN_NOT_OK(FinishCurDataBlock());
    }
  }

  DCHECK_EQ(rem, 0);
  return Status::OK();
}

Status CFileWriter::AppendNullableEntries(const uint8_t *bitmap,
                                          const void *entries,
                                          size_t count) {
  DCHECK(is_nullable_ && bitmap != NULL);

  const uint8_t *ptr = reinterpret_cast<const uint8_t *>(entries);

  size_t nblock;
  bool not_null = false;
  BitmapIterator bmap_iter(bitmap, count);
  while ((nblock = bmap_iter.Next(&not_null)) > 0) {
    if (not_null) {
      size_t rem = nblock;
      do {
        int n = data_block_->Add(ptr, rem);
        DCHECK_GE(n, 0);

        null_bitmap_builder_->AddRun(true, n);
        ptr += n * typeinfo_->size();
        value_count_ += n;
        rem -= n;

        size_t est_size = data_block_->EstimateEncodedSize();
        if (est_size > options_.block_size) {
          RETURN_NOT_OK(FinishCurDataBlock());
        }
      } while (rem > 0);
    } else {
      null_bitmap_builder_->AddRun(false, nblock);
      ptr += nblock * typeinfo_->size();
      value_count_ += nblock;
    }
  }

  return Status::OK();
}

Status CFileWriter::FinishCurDataBlock() {
  uint32_t num_elems_in_block = data_block_->Count();
  if (is_nullable_) {
    num_elems_in_block = null_bitmap_builder_->nitems();
  }

  if (PREDICT_FALSE(num_elems_in_block == 0)) {
    return Status::OK();
  }

  rowid_t first_elem_ord = value_count_ - num_elems_in_block;
  VLOG(1) << "Appending data block for values " <<
    first_elem_ord << "-" << (first_elem_ord + num_elems_in_block);

  // The current data block is full, need to push it
  // into the file, and add to index
  Slice data = data_block_->Finish(first_elem_ord);
  VLOG(2) << "estimated size=" << data_block_->EstimateEncodedSize()
          << " actual=" << data.size();

  uint8_t key_tmp_space[typeinfo_->size()];

  if (validx_builder_ != NULL) {
    // If we're building an index, we need to copy the first
    // key from the block locally, so we can write it into that index.
    RETURN_NOT_OK(data_block_->GetFirstKey(key_tmp_space));
    VLOG(1) << "Appending validx entry\n" <<
      kudu::HexDump(Slice(key_tmp_space, typeinfo_->size()));
  }

  vector<Slice> v;
  faststring null_headers;
  if (is_nullable_) {
    Slice null_bitmap = null_bitmap_builder_->Finish();
    PutVarint32(&null_headers, num_elems_in_block);
    PutVarint32(&null_headers, null_bitmap.size());
    v.push_back(Slice(null_headers.data(), null_headers.size()));
    v.push_back(null_bitmap);
  }
  v.push_back(data);
  Status s = AppendRawBlock(v, first_elem_ord,
                            reinterpret_cast<const void *>(key_tmp_space),
                            "data block");

  if (is_nullable_) {
    null_bitmap_builder_->Reset();
  }
  data_block_->Reset();

  return s;
}

Status CFileWriter::AppendRawBlock(const vector<Slice> &data_slices,
                                   size_t ordinal_pos,
                                   const void *validx_key,
                                   const char *name_for_log) {
  CHECK_EQ(state_, kWriterWriting);

  BlockPointer ptr;
  Status s = AddBlock(data_slices, &ptr, name_for_log);
  if (!s.ok()) {
    LOG(WARNING) << "Unable to append block to file: " << s.ToString();
    return s;
  }

  // Now add to the index blocks
  if (posidx_builder_ != NULL) {
    tmp_buf_.clear();
    KeyEncoderTraits<UINT32>::Encode(ordinal_pos, &tmp_buf_);
    RETURN_NOT_OK(posidx_builder_->Append(Slice(tmp_buf_), ptr));
  }

  if (validx_builder_ != NULL) {
    CHECK(validx_key != NULL) <<
      "must pass a  key for raw block if validx is configured";
    VLOG(1) << "Appending validx entry\n" <<
      kudu::HexDump(Slice(reinterpret_cast<const uint8_t *>(validx_key),
                          typeinfo_->size()));
    key_encoder_.ResetAndEncode(validx_key, &tmp_buf_);
    s = validx_builder_->Append(Slice(tmp_buf_), ptr);
    if (!s.ok()) {
      LOG(WARNING) << "Unable to append to value index: " << s.ToString();
      return s;
    }
  }

  return s;
}

size_t CFileWriter::written_size() const {
  // This is a low estimate, but that's OK -- this is checked after every block
  // write during flush/compact, so better to give a fast slightly-inaccurate result
  // than spend a lot of effort trying to improve accuracy by a few KB.
  return off_;
}

Status CFileWriter::AddBlock(const vector<Slice> &data_slices,
                             BlockPointer *block_ptr,
                             const char *name_for_log) {
  uint64_t start_offset = off_;

  if (block_compressor_ != NULL) {
    // Write compressed block
    Slice cdata;
    Status s = block_compressor_->Compress(data_slices, &cdata);
    if (!s.ok()) {
      LOG(WARNING) << "Unable to compress slice of size "
                   << cdata.size() << " at offset " << off_
                   << ": " << s.ToString();
      return(s);
    }

    RETURN_NOT_OK(WriteRawData(cdata));
  } else {
    // Write uncompressed block
    BOOST_FOREACH(const Slice &data, data_slices) {
      RETURN_NOT_OK(WriteRawData(data));
    }
  }

  uint64_t total_size = off_ - start_offset;

  *block_ptr = BlockPointer(start_offset, total_size);
  VLOG(1) << "Appended " << name_for_log
          << " with " << total_size << " bytes at " << start_offset;
  return Status::OK();
}

Status CFileWriter::WriteRawData(const Slice& data) {
  Status s = file_->Append(data);
  if (!s.ok()) {
    LOG(WARNING) << "Unable to append slice of size "
                << data.size() << " at offset " << off_
                << ": " << s.ToString();
  }
  off_ += data.size();
  return s;
}

CFileWriter::~CFileWriter() {
}

} // namespace cfile
} // namespace kudu