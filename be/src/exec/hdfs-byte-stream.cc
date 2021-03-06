// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exec/hdfs-byte-stream.h"

#include <sstream>
#include "util/hdfs-util.h"
#include "common/status.h"

using namespace impala;
using namespace std;

HdfsByteStream::HdfsByteStream(hdfsFS hdfs_connection, HdfsScanNode* scan_node)
    : ByteStream(),
      hdfs_connection_(hdfs_connection),
      hdfs_file_(NULL),
      scan_node_(scan_node) {
}

Status HdfsByteStream::GetPosition(int64_t* position) {
  // TODO: Deal with error codes from hdfsTell
  DCHECK(hdfs_file_ != NULL);
  *position = hdfsTell(hdfs_connection_, hdfs_file_);
  DCHECK_NE(*position, -1);
  return Status::OK;
}

Status HdfsByteStream::Open(const string& location) {
  DCHECK(hdfs_file_ == NULL);
  location_ = location;
  hdfs_file_ = hdfsOpenFile(hdfs_connection_, location_.c_str(), O_RDONLY, 0, 0, 0);
  if (hdfs_file_ == NULL) {
    return Status(AppendHdfsErrorMessage("Failed to open HDFS file ", location_));
  }
  VLOG_FILE << "HdfsByteStream: opened file " << location_;
  return Status::OK;
}

Status HdfsByteStream::Read(uint8_t* buf, int64_t req_length, int64_t* actual_length) {
  DCHECK(buf != NULL);
  DCHECK(req_length >= 0);

  int n_read = 0;
  while (n_read < req_length) {
    SCOPED_TIMER(scan_node_->read_timer());
    int last_read =
      hdfsRead(hdfs_connection_, hdfs_file_, buf + n_read, req_length - n_read);
    if (last_read == 0) {
      *actual_length = n_read;
      return Status::OK;
    } else if (last_read == -1) {
      // In case of error, *actual_length is not updated
      return Status(AppendHdfsErrorMessage("Error reading from HDFS file: ", location_));
    }

    n_read += last_read;
  }

  COUNTER_UPDATE(scan_node_->bytes_read_counter(), n_read);
  total_bytes_read_ += n_read;
  *actual_length = n_read;
  return Status::OK;
}

Status HdfsByteStream::Close() {
  if (hdfs_file_ == NULL) return Status::OK;
  int hdfs_ret = hdfsCloseFile(hdfs_connection_, hdfs_file_);
  if (hdfs_ret != 0) {
    return Status(AppendHdfsErrorMessage("Error closing HDFS file: ", location_));
  }
  hdfs_file_ = NULL;
  return Status::OK;
}

Status HdfsByteStream::Seek(int64_t offset) {
  DCHECK(hdfs_file_ != NULL);
  if (hdfsSeek(hdfs_connection_, hdfs_file_, offset) != 0) {
    return Status(AppendHdfsErrorMessage("Error seeking HDFS file: " + location_));
  }

  return Status::OK;
}

Status HdfsByteStream::SeekRelative(int64_t offset) {
  DCHECK(hdfs_file_ != NULL);
  int64_t position = hdfsTell(hdfs_connection_, hdfs_file_);
  DCHECK_NE(position, -1);

  if (hdfsSeek(hdfs_connection_, hdfs_file_, position + offset) != 0) {
    return Status(AppendHdfsErrorMessage("Error seeking HDFS file: " + location_));
  }

  return Status::OK;
}

Status HdfsByteStream::Eof(bool* eof) {
  hdfsFileInfo* hdfsInfo = hdfsGetPathInfo(hdfs_connection_, &location_[0]);
  if (hdfsInfo == NULL) {
    return Status(AppendHdfsErrorMessage("Error getting Info for HDFS file: " + location_));
  }
  *eof = hdfsTell(hdfs_connection_, hdfs_file_) >= hdfsInfo->mSize;

  hdfsFreeFileInfo(hdfsInfo, 1);
  return Status::OK;
}
