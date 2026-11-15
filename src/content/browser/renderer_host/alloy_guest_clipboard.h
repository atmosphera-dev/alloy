// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_BROWSER_RENDERER_HOST_ALLOY_GUEST_CLIPBOARD_H_
#define CONTENT_BROWSER_RENDERER_HOST_ALLOY_GUEST_CLIPBOARD_H_

#include "content/public/browser/alloy_guest_integration.h"
#include "content/public/browser/weak_document_ptr.h"
#include "ui/base/clipboard/clipboard.h"

namespace content {

// A document-scoped clipboard backend, injected into the existing native
// ClipboardHost and writer. Native permission, DLP, sanitization and editing
// remain their owners; this class only replaces the OS/backend dependency.
class AlloyGuestClipboard final
    : public ui::Clipboard,
      public std::enable_shared_from_this<AlloyGuestClipboard> {
 public:
  AlloyGuestClipboard(RenderFrameHost* frame,
                       base::RepeatingCallback<bool()> receiver_admitted);
  ~AlloyGuestClipboard() override;
  void RefreshSequence(ui::ClipboardBuffer buffer,
                          base::OnceCallback<void(bool)> done);
  // The policy owner supplies its original token, not this backend's mutable
  // cache: concurrent clipboard reads may refresh that cache independently.
  void ConfirmReadSequence(ui::ClipboardBuffer buffer, ui::ClipboardSequenceNumberToken expected,
      base::OnceCallback<void(bool)> done) const;
  void OnPreShutdown() override;
  void GetSource(ui::ClipboardBuffer, GetSourceCallback callback) const override;
  const ui::ClipboardSequenceNumberToken& GetSequenceNumber(
      ui::ClipboardBuffer buffer) const override;
  void GetStandardFormats(ui::ClipboardBuffer buffer,
      const std::optional<ui::DataTransferEndpoint>&,
      GetStandardFormatsCallback callback) const override;
  void GetAllAvailableFormats(ui::ClipboardBuffer buffer,
      const std::optional<ui::DataTransferEndpoint>&,
      base::OnceCallback<void(base::flat_set<ui::ClipboardFormatType>)>
          callback) const override;
  void Clear(ui::ClipboardBuffer buffer) override;
  void ReadAvailableTypes(ui::ClipboardBuffer buffer,
      const std::optional<ui::DataTransferEndpoint>&,
      ReadAvailableTypesCallback callback) const override;
  void ReadText(ui::ClipboardBuffer buffer,
      const std::optional<ui::DataTransferEndpoint>&,
      ReadTextCallback callback) const override;
  void ReadAsciiText(ui::ClipboardBuffer buffer,
      const std::optional<ui::DataTransferEndpoint>&,
      ReadAsciiTextCallback callback) const override;
  void ReadHTML(ui::ClipboardBuffer buffer,
      const std::optional<ui::DataTransferEndpoint>&,
      ReadHtmlCallback callback) const override;
  void ReadSvg(ui::ClipboardBuffer buffer,
      const std::optional<ui::DataTransferEndpoint>&,
      ReadSvgCallback callback) const override;
  void ReadRTF(ui::ClipboardBuffer buffer,
      const std::optional<ui::DataTransferEndpoint>&,
      ReadRTFCallback callback) const override;
  void ReadPng(ui::ClipboardBuffer buffer,
      const std::optional<ui::DataTransferEndpoint>&,
      ReadPngCallback callback) const override;
  void ReadDataTransferCustomData(ui::ClipboardBuffer buffer,
      const std::u16string& type,
      const std::optional<ui::DataTransferEndpoint>&,
      ReadDataTransferCustomDataCallback callback) const override;
  void ReadFilenames(ui::ClipboardBuffer buffer,
      const std::optional<ui::DataTransferEndpoint>&,
      ReadFilenamesCallback callback) const override;
  void ReadURL(const std::optional<ui::DataTransferEndpoint>&,
      ReadUrlCallback callback) const override;
  void ReadData(const ui::ClipboardFormatType& format,
      const std::optional<ui::DataTransferEndpoint>&,
      ReadDataCallback callback) const override;

 private:
  using Data = AlloyGuestIntegrationData;
  using Result = std::optional<Data>;
  void Read(ui::ClipboardBuffer buffer, std::string format,
      AlloyGuestIntegration::Reply reply, base::TimeTicks deadline = {},
      std::optional<ui::ClipboardSequenceNumberToken> expected = std::nullopt) const;
  void ReadTextBytes(ui::ClipboardBuffer buffer, AlloyGuestIntegration::Reply reply) const;
  void CaptureSequence(ui::ClipboardBuffer buffer, base::RepeatingCallback<bool()> admitted,
      base::OnceCallback<void(std::optional<ui::ClipboardSequenceNumberToken>)> reply) const;
  void ConfirmSequence(ui::ClipboardBuffer buffer, ui::ClipboardSequenceNumberToken expected,
      base::RepeatingCallback<bool()> admitted, base::OnceCallback<void(bool)> reply) const;
  void ReadFilenamesAtSequence(ui::ClipboardBuffer buffer, ui::ClipboardSequenceNumberToken expected,
      base::RepeatingCallback<bool()> admitted, ReadFilenamesCallback callback) const;
  void Send(AlloyGuestIntegration::Operation operation, Data data,
             AlloyGuestIntegration::Reply reply) const;
  void Add(std::string format, std::string bytes);
  void WritePortableAndPlatformRepresentations(ui::ClipboardBuffer buffer,
      const ObjectMap& objects, const std::vector<RawData>& raw_objects,
      std::vector<PlatformRepresentation> platform_representations,
      std::unique_ptr<ui::DataTransferEndpoint> data_src,
      uint32_t privacy_types) override;
  void WriteText(std::string_view text) override;
  void WriteHTML(std::string_view markup,
                 std::optional<std::string_view> url) override;
  void WriteSvg(std::string_view markup) override;
  void WriteRTF(std::string_view rtf) override;
  void WriteFilenames(std::vector<ui::FileInfo> files) override;
  void WriteURL(const ui::ClipboardUrlInfo& url) override;
  void WriteWebSmartPaste() override;
  void WriteBitmap(const SkBitmap& bitmap) override;
  void WriteData(const ui::ClipboardFormatType& format,
                 base::span<const uint8_t> data) override;
#if BUILDFLAG(IS_OZONE)
  bool IsSelectionBufferAvailable() const override;
#endif
  const WeakDocumentPtr document_;
  const base::RepeatingCallback<bool()> receiver_admitted_;
  ui::ClipboardSequenceNumberToken sequence_[2];
  Data transaction_;
  bool transaction_valid_ = false;
  mutable base::WeakPtrFactory<AlloyGuestClipboard> weak_{this};
};

}  // namespace content
#endif
