// Copyright 2014 The Bazel Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "src/main/cpp/archive_utils.h"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "src/main/cpp/blaze_util_platform.h"
#include "src/main/cpp/util/errors.h"
#include "src/main/cpp/util/exit_code.h"
#include "src/main/cpp/util/file.h"
#include "src/main/cpp/util/logging.h"
#include "src/main/cpp/util/path.h"
#include "src/main/cpp/util/strings.h"
#include "third_party/ijar/zip.h"

namespace blaze {

using std::set;
using std::string;
using std::vector;

struct PartialZipExtractor : public devtools_ijar::ZipExtractorProcessor {
  using CallbackType =
      std::function<void(const char *name, const char *data, size_t size)>;

  // Scan the zip file "archive_path" until a file named "stop_entry" is seen,
  // then stop.
  // If entry_names is not nullptr, it receives a list of all file members
  // up to and including "stop_entry".
  // If a callback is given, it is run with the name and contents of
  // each such member.
  // Returns the contents of the "stop_entry" member.
  string UnzipUntil(const string &archive_path, const string &stop_entry,
                    vector<string> *entry_names = nullptr,
                    CallbackType &&callback = {}) {
    std::unique_ptr<devtools_ijar::ZipExtractor> extractor(
        devtools_ijar::ZipExtractor::Create(archive_path.c_str(), this));
    if (!extractor) {
      BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR)
          << "Failed to open '" << archive_path
          << "' as a zip file: " << blaze_util::GetLastErrorString();
    }
    stop_name_ = stop_entry;
    seen_names_.clear();
    callback_ = callback;
    done_ = false;
    while (!done_ && extractor->ProcessNext()) {
      // Scan zip until EOF, an error, or Accept() has seen stop_entry.
    }
    if (const char *err = extractor->GetError()) {
      BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR)
          << "Error reading zip file '" << archive_path << "': " << err;
    }
    if (!done_) {
      BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR)
          << "Failed to find member '" << stop_entry << "' in zip file '"
          << archive_path << "'";
    }
    if (entry_names) *entry_names = std::move(seen_names_);
    return stop_value_;
  }

  bool Accept(const char *filename, devtools_ijar::u4 attr) override {
    if (devtools_ijar::zipattr_is_dir(attr)) return false;
    // Sometimes that fails to detect directories.  Check the name too.
    string fn = filename;
    if (fn.empty() || fn.back() == '/') return false;
    if (stop_name_ == fn) done_ = true;
    seen_names_.push_back(std::move(fn));
    return done_ || !!callback_;  // true if a callback was supplied
  }

  void Process(const char *filename, devtools_ijar::u4 attr,
               const devtools_ijar::u1 *data, size_t size) override {
    if (done_) {
      stop_value_.assign(reinterpret_cast<const char *>(data), size);
    }
    if (callback_) {
      callback_(filename, reinterpret_cast<const char *>(data), size);
    }
  }

  string stop_name_;
  string stop_value_;
  vector<string> seen_names_;
  CallbackType callback_;
  bool done_ = false;
};

void DetermineArchiveContents(const string &archive_path, vector<string> *files,
                              string *install_md5) {
  PartialZipExtractor pze;
  *install_md5 = pze.UnzipUntil(archive_path, "install_base_key", files);
}

void ExtractArchiveOrDie(const string &archive_path, const string &product_name,
                         const string &expected_install_md5,
                         const string &output_dir) {
  string error;
  std::unique_ptr<blaze::embedded_binaries::Dumper> dumper(
      blaze::embedded_binaries::Create(&error));
  if (dumper == nullptr) {
    BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR) << error;
  }

  if (!blaze_util::PathExists(output_dir)) {
    BAZEL_DIE(blaze_exit_code::INTERNAL_ERROR)
        << "Archive output directory didn't exist: " << output_dir;
  }

  BAZEL_LOG(USER) << "Extracting " << product_name << " installation...";

  PartialZipExtractor pze;
  string install_md5 = pze.UnzipUntil(
      archive_path, "install_base_key", nullptr,
      [&](const char *name, const char *data, size_t size) {
        dumper->Dump(data, size, blaze_util::JoinPath(output_dir, name));
      });

  if (!dumper->Finish(&error)) {
    BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR)
        << "Failed to extract embedded binaries: " << error;
  }

  if (install_md5 != expected_install_md5) {
    BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR)
        << "The " << product_name << " binary at " << archive_path
        << " was replaced during the client's self-extraction (old md5: "
        << expected_install_md5 << " new md5: " << install_md5
        << "). If you expected this then you should simply re-run "
        << product_name
        << " in order to pick up the different version. If you didn't expect "
           "this then you should investigate what happened.";
  }
}

void BlessFiles(const string &embedded_binaries) {
  blaze_util::Path embedded_binaries_(embedded_binaries);

  // Set the timestamps of the extracted files to the future and make sure (or
  // at least as sure as we can...) that the files we have written are actually
  // on the disk.

  vector<string> extracted_files;

  // Walks the temporary directory recursively and collects full file paths.
  blaze_util::GetAllFilesUnder(embedded_binaries, &extracted_files);

  std::unique_ptr<blaze_util::IFileMtime> mtime(blaze_util::CreateFileMtime());
  set<blaze_util::Path> synced_directories;
  for (const auto &f : extracted_files) {
    blaze_util::Path it(f);

    // Set the time to a distantly futuristic value so we can observe tampering.
    // Note that keeping a static, deterministic timestamp, such as the default
    // timestamp set by unzip (1970-01-01) and using that to detect tampering is
    // not enough, because we also need the timestamp to change between Bazel
    // releases so that the metadata cache knows that the files may have
    // changed. This is essential for the correctness of actions that use
    // embedded binaries as artifacts.
    if (!mtime->SetToDistantFuture(it)) {
      string err = blaze_util::GetLastErrorString();
      BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR)
          << "failed to set timestamp on '" << it.AsPrintablePath()
          << "': " << err;
    }

    blaze_util::SyncFile(it);

    blaze_util::Path directory = it.GetParent();

    // Now walk up until embedded_binaries and sync every directory in between.
    // synced_directories is used to avoid syncing the same directory twice.
    // The !directory.empty() and !blaze_util::IsRootDirectory(directory)
    // conditions are not strictly needed, but it makes this loop more robust,
    // because otherwise, if due to some glitch, directory was not under
    // embedded_binaries, it would get into an infinite loop.
    while (directory != embedded_binaries_ && !directory.IsEmpty() &&
           !blaze_util::IsRootDirectory(directory) &&
           synced_directories.insert(directory).second) {
      blaze_util::SyncFile(directory);
      directory = directory.GetParent();
    }
  }

  blaze_util::SyncFile(embedded_binaries_);
}

void ExtractBuildLabel(const string &archive_path, string *build_label) {
  PartialZipExtractor pze;
  *build_label = pze.UnzipUntil(archive_path, "build-label.txt");
}

string GetServerJarPath(const vector<string> &archive_contents) {
  if (archive_contents.empty()) {
    BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR)
        << "Couldn't find server jar in archive";
  }
  return archive_contents[0];
}

}  // namespace blaze
