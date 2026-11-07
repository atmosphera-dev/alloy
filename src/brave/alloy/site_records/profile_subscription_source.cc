// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_subscription_source.h"

#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/strings/escape.h"
#include "base/strings/string_view_util.h"

namespace alloy::site_records {
namespace {
using Provider=brave_shields::AdBlockSubscriptionFiltersProvider;
// The native data source takes ownership of this response. Clear its private
// payload when that native response is retired, rather than caching source text
// in a global data source, resource map or navigation URL.
class SourceResponse final : public base::RefCountedMemory {
 public:
  explicit SourceResponse(std::string html):html_(std::move(html)) {}
 private:
  ~SourceResponse() override {Cleanse(base::as_writable_byte_span(html_));}
  base::span<const uint8_t> AsSpan() const override {return base::as_byte_span(html_);}
  std::string html_;
};
}  // namespace

bool IsProfileSubscriptionSourcePath(const std::string& path) {
  return path.starts_with("subscription-source/");
}

content::WebUIDataSource::HandleRequestCallback ProfileSubscriptionSourceHandler(
    scoped_refptr<ProfileFilterPreferences> original) {
  return base::BindRepeating([](scoped_refptr<ProfileFilterPreferences> original,
      const std::string& path,content::WebUIDataSource::GotDataCallback reply) {
    if(!original || !original->available() || !IsProfileSubscriptionSourcePath(path)) {
      std::move(reply).Run(nullptr);return;
    }
    original->ReadSubscriptionSource(path,base::BindOnce(
        [](scoped_refptr<ProfileFilterPreferences> original,
           content::WebUIDataSource::GotDataCallback reply,Provider::SourceResult status,
           DATFileDataBuffer bytes) {
          if(status!=Provider::SourceResult::kOk || !original->available()) {
            Cleanse(bytes);std::move(reply).Run(nullptr);return;
          }
          // WebUIDataSource serves unknown extensions as text/html, including
          // .txt. Escape the complete filter text in an inert preformatted page.
          auto escaped=base::EscapeForHTML(base::as_string_view(bytes));Cleanse(bytes);
          std::string html="<!doctype html><meta charset=utf-8><title>Filter list source</title><pre>";
          html+=escaped;html+="</pre>";Cleanse(base::as_writable_byte_span(escaped));
          std::move(reply).Run(base::MakeRefCounted<SourceResponse>(std::move(html)));
        },original,std::move(reply)));
  },std::move(original));
}

void InstallProfileSubscriptionSource(content::WebUIDataSource& source,
    scoped_refptr<ProfileFilterPreferences> original) {
  source.SetRequestFilter(base::BindRepeating(&IsProfileSubscriptionSourcePath),
      ProfileSubscriptionSourceHandler(std::move(original)));
}
}  // namespace alloy::site_records
