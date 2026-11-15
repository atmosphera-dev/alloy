// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/browser/renderer_host/alloy_guest_clipboard.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/pickle.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/unguessable_token.h"
#include "content/public/browser/alloy_document_state.h"
#include "content/public/browser/clipboard_types.h"
#include "content/browser/file_system_access/alloy_guest_uploads.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "storage/browser/file_system/file_system_context.h"
#include "ui/base/clipboard/clipboard_constants.h"
#include "ui/base/clipboard/custom_data_helper.h"
#include "ui/gfx/codec/png_codec.h"

namespace content {
namespace {
using Data = AlloyGuestIntegrationData;
using Result = std::optional<Data>;
using Operation = AlloyGuestIntegration::Operation;

std::string Buffer(ui::ClipboardBuffer buffer) {
  return buffer == ui::ClipboardBuffer::kCopyPaste ? "copy-paste" : "selection";
}
std::string Bytes(Result result) {
  return result && result->bytes.size() == 1 ? std::move(result->bytes[0])
                                           : std::string();
}
std::vector<std::u16string> Types(Result result) {
  std::vector<std::u16string> types;
  const auto* values = result ? result->details.FindList("formats") : nullptr;
  if (!values) return types;
  for (const auto& value : *values) {
    if (!value.is_string() || value.GetString().empty() ||
        !base::IsStringASCII(value.GetString()) || value.GetString().find('\0') != std::string::npos)
      return {};
    types.push_back(base::UTF8ToUTF16(value.GetString()));
  }
  return types;
}
std::vector<std::u16string> StandardTypes(const std::vector<std::u16string>& native) {
  std::vector<std::u16string> result;
  for (const auto& type : native) {
    std::u16string mime;
    if (type == u"text/plain" || type == u"TEXT" || type == u"STRING" ||
        type == u"UTF8_STRING" || type == u"text/plain;charset=utf-8") mime = u"text/plain";
    else if (type == u"text/html" || type == u"image/svg+xml" || type == u"text/rtf" ||
             type == u"image/png" || type == u"text/uri-list") mime = type;
    if (!mime.empty() && std::find(result.begin(), result.end(), mime) == result.end())
      result.push_back(std::move(mime));
  }
  return result;
}
}  // namespace

AlloyGuestClipboard::AlloyGuestClipboard(
    RenderFrameHost* frame, base::RepeatingCallback<bool()> receiver_admitted)
    : document_(frame ? frame->GetWeakDocumentPtr() : WeakDocumentPtr()),
      receiver_admitted_(std::move(receiver_admitted)) {}
AlloyGuestClipboard::~AlloyGuestClipboard() = default;

void AlloyGuestClipboard::Send(Operation operation, Data data,
                                AlloyGuestIntegration::Reply reply) const {
  auto* frame = document_.AsRenderFrameHostIfValid();
  auto* bridge = AlloyGuestIntegration::Get(frame);
  if (!frame || !frame->IsActive() || !AlloyDocumentStateAdmitted(frame) || !bridge ||
      !receiver_admitted_.Run()) {
    std::move(reply).Run(std::nullopt);
    return;
  }
  data.admitted = receiver_admitted_;
  // ui::Clipboard helpers may retain a raw backend pointer across their async
  // read chain, because ordinary backends are browser-lifetime objects. Retain
  // this scoped backend through each continuation, without retaining the
  // receiver's admission or its native document.
  bridge->Request(frame, operation, std::move(data), base::BindOnce(
      [](std::shared_ptr<const AlloyGuestClipboard>,
          AlloyGuestIntegration::Reply reply, Result result) {
        std::move(reply).Run(std::move(result));
      }, shared_from_this(), std::move(reply)));
}

void AlloyGuestClipboard::Read(ui::ClipboardBuffer buffer, std::string format,
                                AlloyGuestIntegration::Reply reply) const {
  Data data;
  data.details.Set("buffer", Buffer(buffer));
  data.details.Set("format", std::move(format));
  Send(Operation::kClipboardRead, std::move(data), std::move(reply));
}

void AlloyGuestClipboard::RefreshSequence(ui::ClipboardBuffer buffer,
                                           base::OnceCallback<void(bool)> done) {
  Data data;
  data.details.Set("buffer", Buffer(buffer));
  // document_ may expire while the app decides. The request owner completes
  // denied on retirement; the ClipboardHost weak callback owns continuation.
  Send(Operation::kClipboardSequence, std::move(data), base::BindOnce(
      [](base::WeakPtr<AlloyGuestClipboard> self, ui::ClipboardBuffer buffer,
          base::OnceCallback<void(bool)> done, Result result) {
        bool valid = false;
        if (self && result) {
          const auto* text = result->details.FindString("sequence");
          auto token = text ? base::UnguessableToken::DeserializeFromString(*text)
                            : std::nullopt;
          if (token) {
            self->sequence_[buffer == ui::ClipboardBuffer::kCopyPaste ? 0 : 1] =
                ui::ClipboardSequenceNumberToken(*token);
            valid = true;
          }
        }
        std::move(done).Run(valid);
      }, weak_.GetWeakPtr(), buffer, std::move(done)));
}

void AlloyGuestClipboard::OnPreShutdown() {}
void AlloyGuestClipboard::GetSource(ui::ClipboardBuffer,
                                     GetSourceCallback callback) const {
  // The app owns the OS clipboard. Never reinterpret app-supplied bytes as a
  // native RFH token or look up a replacement document's clipboard authority.
  std::move(callback).Run(std::nullopt);
}
const ui::ClipboardSequenceNumberToken& AlloyGuestClipboard::GetSequenceNumber(
    ui::ClipboardBuffer buffer) const {
  return sequence_[buffer == ui::ClipboardBuffer::kCopyPaste ? 0 : 1];
}
void AlloyGuestClipboard::GetStandardFormats(ui::ClipboardBuffer buffer,
    const std::optional<ui::DataTransferEndpoint>&,
    GetStandardFormatsCallback callback) const {
  Read(buffer, "@types", base::BindOnce(
      [](GetStandardFormatsCallback callback, Result result) {
        std::move(callback).Run(StandardTypes(Types(std::move(result))));
      }, std::move(callback)));
}
void AlloyGuestClipboard::GetAllAvailableFormats(ui::ClipboardBuffer buffer,
    const std::optional<ui::DataTransferEndpoint>&,
    base::OnceCallback<void(base::flat_set<ui::ClipboardFormatType>)> callback) const {
  Read(buffer, "@types", base::BindOnce(
      [](base::OnceCallback<void(base::flat_set<ui::ClipboardFormatType>)> callback,
          Result result) {
        base::flat_set<ui::ClipboardFormatType> formats;
        for (const auto& type : Types(std::move(result)))
          formats.insert(ui::ClipboardFormatType::CustomPlatformType(base::UTF16ToUTF8(type)));
        std::move(callback).Run(std::move(formats));
      }, std::move(callback)));
}
void AlloyGuestClipboard::Clear(ui::ClipboardBuffer buffer) {
  Data data;
  data.details.Set("buffer", Buffer(buffer));
  data.details.Set("formats", base::ListValue());
  Send(Operation::kClipboardWrite, std::move(data), base::DoNothing());
}
void AlloyGuestClipboard::ReadAvailableTypes(ui::ClipboardBuffer buffer,
    const std::optional<ui::DataTransferEndpoint>&,
    ReadAvailableTypesCallback callback) const {
  Read(buffer, "@types", base::BindOnce(
      [](std::shared_ptr<const AlloyGuestClipboard> self, ui::ClipboardBuffer buffer,
          ReadAvailableTypesCallback callback, Result result) {
        const auto native = Types(std::move(result));
        auto standard = StandardTypes(native);
        const auto custom = base::UTF8ToUTF16(ui::ClipboardFormatType::DataTransferCustomType().GetName());
        if (std::find(native.begin(), native.end(), custom) == native.end()) {
          std::move(callback).Run(std::move(standard)); return;
        }
        self->Read(buffer, ui::ClipboardFormatType::DataTransferCustomType().GetName(),
            base::BindOnce([](ReadAvailableTypesCallback callback,
                              std::vector<std::u16string> types, Result result) {
              auto bytes = Bytes(std::move(result));
              ui::ReadCustomDataTypes(base::as_byte_span(bytes), &types);
              std::move(callback).Run(std::move(types));
            }, std::move(callback), std::move(standard)));
      }, shared_from_this(), buffer, std::move(callback)));
}
void AlloyGuestClipboard::ReadText(ui::ClipboardBuffer buffer,
    const std::optional<ui::DataTransferEndpoint>&,
    ReadTextCallback callback) const {
  Read(buffer, "text/plain", base::BindOnce(
      [](ReadTextCallback callback, Result result) {
        std::move(callback).Run(base::UTF8ToUTF16(Bytes(std::move(result))));
      }, std::move(callback)));
}
void AlloyGuestClipboard::ReadAsciiText(ui::ClipboardBuffer buffer,
    const std::optional<ui::DataTransferEndpoint>&,
    ReadAsciiTextCallback callback) const {
  Read(buffer, "text/plain", base::BindOnce(
      [](ReadAsciiTextCallback callback, Result result) {
        std::move(callback).Run(Bytes(std::move(result)));
      }, std::move(callback)));
}
void AlloyGuestClipboard::ReadHTML(ui::ClipboardBuffer buffer,
    const std::optional<ui::DataTransferEndpoint>&,
    ReadHtmlCallback callback) const {
  Read(buffer, "text/html", base::BindOnce(
      [](ReadHtmlCallback callback, Result result) {
        GURL url;
        std::u16string markup;
        uint32_t start = 0, end = 0;
        if (result && result->bytes.size() == 1) {
          markup = base::UTF8ToUTF16(result->bytes[0]);
          const auto* source = result->details.FindString("source-url");
          if (source) url = GURL(*source);
          const auto* first = result->details.FindString("fragment-start");
          const auto* last = result->details.FindString("fragment-end");
          if (!first || !last || !base::StringToUint(*first, &start) ||
              !base::StringToUint(*last, &end) || start > end || end > markup.size()) {
            markup.clear(); url = GURL(); start = end = 0;
          }
        }
        std::move(callback).Run(std::move(markup), std::move(url), start, end);
      }, std::move(callback)));
}
void AlloyGuestClipboard::ReadSvg(ui::ClipboardBuffer buffer,
    const std::optional<ui::DataTransferEndpoint>&,
    ReadSvgCallback callback) const {
  Read(buffer, "image/svg+xml", base::BindOnce(
      [](ReadSvgCallback callback, Result result) {
        std::move(callback).Run(base::UTF8ToUTF16(Bytes(std::move(result))));
      }, std::move(callback)));
}
void AlloyGuestClipboard::ReadRTF(ui::ClipboardBuffer buffer,
    const std::optional<ui::DataTransferEndpoint>&,
    ReadRTFCallback callback) const {
  Read(buffer, "text/rtf", base::BindOnce(
      [](ReadRTFCallback callback, Result result) {
        std::move(callback).Run(Bytes(std::move(result)));
      }, std::move(callback)));
}
void AlloyGuestClipboard::ReadPng(ui::ClipboardBuffer buffer,
    const std::optional<ui::DataTransferEndpoint>&,
    ReadPngCallback callback) const {
  Read(buffer, "image/png", base::BindOnce(
      [](ReadPngCallback callback, Result result) {
        auto bytes = Bytes(std::move(result));
        std::vector<uint8_t> png(bytes.begin(), bytes.end());
        std::move(callback).Run(png);
      }, std::move(callback)));
}
void AlloyGuestClipboard::ReadDataTransferCustomData(ui::ClipboardBuffer buffer,
    const std::u16string& type, const std::optional<ui::DataTransferEndpoint>&,
    ReadDataTransferCustomDataCallback callback) const {
  Read(buffer, ui::ClipboardFormatType::DataTransferCustomType().GetName(),
      base::BindOnce([](ReadDataTransferCustomDataCallback callback,
                        std::u16string type, Result result) {
        auto bytes = Bytes(std::move(result));
        auto value = ui::ReadCustomDataForType(base::as_byte_span(bytes), type);
        std::move(callback).Run(value.value_or(std::u16string()));
      }, std::move(callback), type));
}
void AlloyGuestClipboard::ReadFilenames(ui::ClipboardBuffer buffer,
    const std::optional<ui::DataTransferEndpoint>&,
    ReadFilenamesCallback callback) const {
  auto* original = document_.AsRenderFrameHostIfValid();
  if (!original || !receiver_admitted_.Run()) { std::move(callback).Run({}); return; }
  auto admitted = base::BindRepeating([](std::shared_ptr<const AlloyGuestClipboard> self,
      base::TimeTicks deadline) {
    return self->receiver_admitted_.Run() && base::TimeTicks::Now() < deadline;
  }, shared_from_this(), base::TimeTicks::Now() + AlloyGuestIntegration::kClipboardReadTimeout);
  base::DictValue details; details.Set("mode", "clipboard-files"); details.Set("buffer", Buffer(buffer));
  AlloyGuestFileCapability::Select(original, std::move(details), base::BindOnce(
      [](std::shared_ptr<const AlloyGuestClipboard> self, base::RepeatingCallback<bool()> admitted, ReadFilenamesCallback callback,
          std::optional<AlloyGuestFileCapability::Entries> entries) {
        if (!entries || !admitted.Run() || entries->empty()) { std::move(callback).Run({}); return; }
        SnapshotAlloyGuestFiles(self->document_.AsRenderFrameHostIfValid(), std::move(*entries), false,
            base::BindOnce([](std::shared_ptr<const AlloyGuestClipboard> self, ReadFilenamesCallback callback,
                std::optional<AlloyGuestUploadFiles> snapshot) {
              std::move(callback).Run(snapshot && self->receiver_admitted_.Run() ? snapshot->files : std::vector<ui::FileInfo>());
            }, self, std::move(callback)), admitted);
      }, shared_from_this(), admitted, std::move(callback)), admitted);
}
void AlloyGuestClipboard::ReadURL(const std::optional<ui::DataTransferEndpoint>&,
    ReadUrlCallback callback) const {
  Read(ui::ClipboardBuffer::kCopyPaste, ui::ClipboardFormatType::UrlType().GetName(), base::BindOnce(
      [](ReadUrlCallback callback, Result result) {
        ui::ClipboardUrlInfo url;
        auto bytes = Bytes(std::move(result));
        if (bytes.size() % 2 == 0) {
          std::u16string bookmark;
          bookmark.reserve(bytes.size() / 2);
          for (size_t index = 0; index < bytes.size(); index += 2)
            bookmark.push_back(static_cast<uint8_t>(bytes[index]) |
                               (static_cast<uint16_t>(static_cast<uint8_t>(bytes[index + 1])) << 8));
          const auto split = bookmark.find(u'\n');
          if (split != std::u16string::npos) {
            url.url = GURL(bookmark.substr(0, split));
            url.title = bookmark.substr(split + 1);
          }
        }
        std::move(callback).Run(std::move(url));
      }, std::move(callback)));
}
void AlloyGuestClipboard::ReadData(const ui::ClipboardFormatType& format,
    const std::optional<ui::DataTransferEndpoint>&,
    ReadDataCallback callback) const {
  if (format == SourceRFHTokenType()) {
    std::move(callback).Run({});
    return;
  }
  Read(ui::ClipboardBuffer::kCopyPaste, format.GetName(), base::BindOnce(
      [](ReadDataCallback callback, Result result) {
        std::move(callback).Run(Bytes(std::move(result)));
      }, std::move(callback)));
}

void AlloyGuestClipboard::Add(std::string format, std::string bytes) {
  if (!transaction_valid_) return;
  transaction_.details.EnsureList("formats")->Append(std::move(format));
  transaction_.bytes.push_back(std::move(bytes));
}
void AlloyGuestClipboard::WritePortableAndPlatformRepresentations(
    ui::ClipboardBuffer buffer, const ObjectMap& objects,
    const std::vector<RawData>& raw_objects,
    std::vector<PlatformRepresentation> platform_representations,
    std::unique_ptr<ui::DataTransferEndpoint>, uint32_t privacy_types) {
  transaction_ = Data();
  transaction_valid_ = true;
  transaction_.details.Set("buffer", Buffer(buffer));
  transaction_.details.Set("privacy", static_cast<int>(privacy_types));
  transaction_.details.Set("formats", base::ListValue());
  for (const auto& [index, object] : objects) DispatchPortableRepresentation(object);
  for (const auto& raw : raw_objects) DispatchPortableRepresentation(raw);
  for (const auto& platform : platform_representations)
    Add(platform.format, std::string(reinterpret_cast<const char*>(platform.data.data()),
                                     platform.data.size()));
  if (transaction_valid_)
    Send(Operation::kClipboardWrite, std::move(transaction_), base::DoNothing());
  transaction_ = Data();
  transaction_valid_ = false;
}
void AlloyGuestClipboard::WriteText(std::string_view text) {
  Add("text/plain", std::string(text));
}
void AlloyGuestClipboard::WriteHTML(std::string_view markup,
                                    std::optional<std::string_view> url) {
  if (url) transaction_.details.Set("source-url", std::string(*url));
  Add("text/html", std::string(markup));
}
void AlloyGuestClipboard::WriteSvg(std::string_view markup) {
  Add("image/svg+xml", std::string(markup));
}
void AlloyGuestClipboard::WriteRTF(std::string_view rtf) {
  Add("text/rtf", std::string(rtf));
}
void AlloyGuestClipboard::WriteFilenames(std::vector<ui::FileInfo> files) {
  auto* original = document_.AsRenderFrameHostIfValid();
  if (!original) { transaction_valid_ = false; return; }
  base::ListValue tokens;
  for (const auto& file : files) {
    const auto url = original->GetStoragePartition()->GetFileSystemContext()->CreateCrackedFileSystemURL(
        blink::StorageKey(), storage::kFileSystemTypeLocal, file.path);
    auto cap = AlloyGuestFileCapability::FromURL(original->GetBrowserContext(), url);
    if (!cap || !cap->Admitted()) { transaction_valid_ = false; return; }
    tokens.Append(cap->metadata().capability);
  }
  transaction_.details.Set("file-capabilities", std::move(tokens));
  Add("@files", {});
}
void AlloyGuestClipboard::WriteURL(const ui::ClipboardUrlInfo& url) {
  std::u16string bookmark = url.url.is_valid() ? base::UTF8ToUTF16(url.url.spec()) : std::u16string();
  bookmark += u'\n'; bookmark += url.title;
  std::string bytes;
  if (bookmark.size() > bytes.max_size() / 2) { transaction_valid_ = false; return; }
  bytes.reserve(bookmark.size() * 2);
  for (char16_t unit : bookmark) {
    bytes.push_back(static_cast<char>(unit & 255));
    bytes.push_back(static_cast<char>(unit >> 8));
  }
  Add(ui::ClipboardFormatType::UrlType().GetName(), std::move(bytes));
}
void AlloyGuestClipboard::WriteWebSmartPaste() {
  Add(ui::ClipboardFormatType::WebKitSmartPasteType().GetName(), {});
}
void AlloyGuestClipboard::WriteBitmap(const SkBitmap& bitmap) {
  auto bytes = gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, false);
  if (!bytes) { transaction_valid_ = false; return; }
  Add("image/png", std::string(bytes->begin(), bytes->end()));
}
void AlloyGuestClipboard::WriteData(const ui::ClipboardFormatType& format,
                                    base::span<const uint8_t> data) {
  if (format == SourceRFHTokenType()) return;
  Add(format.GetName(), std::string(data.begin(), data.end()));
}
#if BUILDFLAG(IS_OZONE)
bool AlloyGuestClipboard::IsSelectionBufferAvailable() const { return true; }
#endif

}  // namespace content
