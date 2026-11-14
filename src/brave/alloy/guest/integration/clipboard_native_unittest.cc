// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/pickle.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/test_future.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "chrome/browser/extensions/chrome_extension_system.h"
#include "chrome/browser/extensions/chrome_extension_system_factory.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/keyed_service/core/keyed_service.h"
#include "content/browser/renderer_host/alloy_guest_clipboard.h"
#include "content/public/browser/alloy_document_state.h"
#include "content/public/test/web_contents_tester.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/clipboard/clipboard_url_info.h"
#include "ui/base/clipboard/custom_data_helper.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/gfx/codec/png_codec.h"
#include "third_party/skia/include/core/SkBitmap.h"

namespace alloy::guest::integration {
namespace {
using IntegrationData = content::AlloyGuestIntegrationData;
constexpr auto kBuffer = ui::ClipboardBuffer::kCopyPaste;

// A synthetic app provider at the native backend boundary. Format conversion
// and ScopedClipboardWriter are production Chromium/Alloy implementations.
class ClipboardProvider final : public content::AlloyGuestIntegration {
 public:
  std::map<std::string, std::string> formats;
  std::string source;
  int calls = 0;
  bool invalid_html = false;
  void Request(content::RenderFrameHost* original, Operation operation,
      IntegrationData data, Reply reply) override {
    ++calls;
    EXPECT_TRUE(content::AlloyDocumentStateAdmitted(original));
    EXPECT_TRUE(data.admitted && data.admitted.Run());
    IntegrationData result;
    if (operation == Operation::kClipboardWrite) {
      const auto* types = data.details.FindList("formats");
      ASSERT_TRUE(types);
      ASSERT_EQ(types->size(), data.bytes.size());
      formats.clear();
      for (size_t index = 0; index < types->size(); ++index)
        formats.emplace((*types)[index].GetString(), std::move(data.bytes[index]));
      source = data.details.FindString("source-url") ? *data.details.FindString("source-url") : "";
    } else if (operation == Operation::kClipboardSequence) {
      result.details.Set("sequence", "00000000000000010000000000000002");
    } else {
      EXPECT_EQ(operation, Operation::kClipboardRead);
      const auto* type = data.details.FindString("format");
      ASSERT_TRUE(type);
      if (*type == "@types") {
        base::ListValue types;
        for (const auto& [name, bytes] : formats) types.Append(name);
        result.details.Set("formats", std::move(types));
      } else {
        result.bytes.push_back(formats[*type]);
        result.details.Set("source-url", source);
        result.details.Set("fragment-start", "0");
        result.details.Set("fragment-end", invalid_html ? "4294967295" :
            std::to_string(base::UTF8ToUTF16(formats[*type]).size()));
      }
    }
    std::move(reply).Run(std::move(result));
  }
  base::RepeatingCallback<bool()> CaptureAdmission(content::RenderFrameHost* original,
                                                    bool) override {
    return base::BindRepeating([](content::WeakDocumentPtr document) {
      return content::AlloyDocumentStateAdmitted(document.AsRenderFrameHostIfValid());
    }, original->GetWeakDocumentPtr());
  }
};

class GuestClipboardNativeTest : public ChromeRenderViewHostTestHarness {
 public:
  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();
    auto* memory = TestingProfile::Builder().BuildOffTheRecord(profile(),
        Profile::OTRProfileID::CreateUniqueForAlloy(site_records::ContextMode::Memory()));
    extensions::ChromeExtensionSystemFactory::GetInstance()->SetTestingFactoryAndUse(
        memory, base::BindRepeating([](content::BrowserContext* context) -> std::unique_ptr<KeyedService> {
          return std::make_unique<extensions::ChromeExtensionSystem>(Profile::FromBrowserContext(context));
        }));
    contents_ = content::WebContentsTester::CreateTestWebContents(memory, nullptr);
    content::WebContentsTester::For(contents_.get())->NavigateAndCommit(GURL("https://clipboard-native.test/"));
    auto provider = std::make_unique<ClipboardProvider>();
    provider_ = provider.get();
    content::AlloyGuestIntegration::Install(contents_.get(), std::move(provider));
    clipboard_ = std::make_shared<content::AlloyGuestClipboard>(contents_->GetPrimaryMainFrame(),
        base::BindRepeating([](bool* live) { return *live; }, &receiver_live_));
  }
  void TearDown() override {
    clipboard_.reset();
    contents_.reset();
    ChromeRenderViewHostTestHarness::TearDown();
  }
 protected:
  std::unique_ptr<content::WebContents> contents_;
  ClipboardProvider* provider_ = nullptr;
  std::shared_ptr<content::AlloyGuestClipboard> clipboard_;
  bool receiver_live_ = true;
};

TEST_F(GuestClipboardNativeTest, NativeWriterFormatsAndOpaqueDataReachProviderAndReadBack) {
  base::Pickle custom;
  ui::WriteCustomDataToPickle(base::flat_map<std::u16string, std::u16string>{
      {u"application/x-alloy-note", u"custom-value"}}, &custom);
  {
    ui::ScopedClipboardWriter writer(kBuffer, nullptr, clipboard_.get());
    writer.WriteText(u"native-\u4e2d\U0001f642");
    writer.WriteHTML(u"<b>native-\u4e2d</b>", "https://source.test/");
    writer.WriteSvg(u"<svg xmlns='http://www.w3.org/2000/svg'/>");
    writer.WriteRTF("{\\rtf1 native-rtf}");
    writer.WritePickledData(custom, ui::ClipboardFormatType::DataTransferCustomType());
    writer.WriteRawDataForTest(ui::ClipboardFormatType::CustomPlatformType("application/x-opaque"),
                               {0, 1, 2, 255});
  }
  ASSERT_EQ(provider_->calls, 1);
  EXPECT_EQ(provider_->formats["text/plain"], "native-\xE4\xB8\xAD\xF0\x9F\x99\x82");
  EXPECT_EQ(provider_->formats["text/rtf"], "{\\rtf1 native-rtf}");
  base::test::TestFuture<std::u16string> text;
  clipboard_->ReadText(kBuffer, std::nullopt, text.GetCallback());
  EXPECT_EQ(text.Get(), u"native-\u4e2d\U0001f642");
  base::test::TestFuture<std::u16string, GURL, uint32_t, uint32_t> html;
  clipboard_->ReadHTML(kBuffer, std::nullopt, html.GetCallback());
  EXPECT_EQ(html.Get<0>(), u"<b>native-\u4e2d</b>");
  EXPECT_EQ(html.Get<1>(), GURL("https://source.test/"));
  EXPECT_EQ(html.Get<2>(), 0u);
  EXPECT_EQ(html.Get<3>(), html.Get<0>().size());
  base::test::TestFuture<std::u16string> svg;
  clipboard_->ReadSvg(kBuffer, std::nullopt, svg.GetCallback());
  EXPECT_EQ(svg.Get(), u"<svg xmlns='http://www.w3.org/2000/svg'/>");
  base::test::TestFuture<std::string> rtf;
  clipboard_->ReadRTF(kBuffer, std::nullopt, rtf.GetCallback());
  EXPECT_EQ(rtf.Get(), "{\\rtf1 native-rtf}");
  base::test::TestFuture<std::u16string> note;
  clipboard_->ReadDataTransferCustomData(kBuffer, u"application/x-alloy-note", std::nullopt, note.GetCallback());
  EXPECT_EQ(note.Get(), u"custom-value");
  base::test::TestFuture<std::string> opaque;
  clipboard_->ReadData(ui::ClipboardFormatType::CustomPlatformType("application/x-opaque"),
                       std::nullopt, opaque.GetCallback());
  EXPECT_EQ(opaque.Get(), std::string("\0\1\2\xff", 4));
  base::test::TestFuture<std::vector<std::u16string>> types;
  clipboard_->ReadAvailableTypes(kBuffer, std::nullopt, types.GetCallback());
  EXPECT_NE(std::find(types.Get().begin(), types.Get().end(), u"application/x-alloy-note"), types.Get().end());
}

TEST_F(GuestClipboardNativeTest, NativeImagePixelsReachAppAndReturnWithoutCanvasReadback) {
  SkBitmap bitmap;
  bitmap.allocN32Pixels(4, 4);
  const SkColor expected = SkColorSetARGB(255, 20, 40, 60);
  bitmap.eraseColor(expected);
  {
    ui::ScopedClipboardWriter writer(kBuffer, nullptr, clipboard_.get());
    writer.WriteImage(bitmap);
  }
  const auto& stored = provider_->formats["image/png"];
  ASSERT_FALSE(stored.empty());
  auto decoded = gfx::PNGCodec::Decode(base::as_byte_span(stored));
  ASSERT_FALSE(decoded.isNull());
  EXPECT_EQ(decoded.width(), 4);
  EXPECT_EQ(decoded.height(), 4);
  EXPECT_EQ(decoded.getColor(0, 0), expected);
  base::test::TestFuture<const std::vector<uint8_t>&> read;
  clipboard_->ReadPng(kBuffer, std::nullopt, read.GetCallback());
  EXPECT_EQ(std::string(read.Get().begin(), read.Get().end()), stored);
}

TEST_F(GuestClipboardNativeTest, NativeBookmarkEncodingAndSequenceArePreserved) {
  ui::ClipboardUrlInfo bookmark;
  bookmark.url = GURL("https://bookmark.test/");
  bookmark.title = u"bookmark-\u4e2d";
  {
    ui::ScopedClipboardWriter writer(kBuffer, nullptr, clipboard_.get());
    writer.WriteURL(bookmark);
  }
  base::test::TestFuture<ui::ClipboardUrlInfo> read;
  clipboard_->ReadURL(std::nullopt, read.GetCallback());
  EXPECT_EQ(read.Get().url, bookmark.url);
  EXPECT_EQ(read.Get().title, bookmark.title);
  const auto before = clipboard_->GetSequenceNumber(kBuffer);
  base::test::TestFuture<bool> sequence;
  clipboard_->RefreshSequence(kBuffer, sequence.GetCallback());
  ASSERT_TRUE(sequence.Get());
  EXPECT_NE(clipboard_->GetSequenceNumber(kBuffer), before);
}

TEST_F(GuestClipboardNativeTest, InvalidHtmlAndRetiredReceiverCannotExposeProviderData) {
  provider_->formats["text/html"] = "<b>original-data</b>";
  provider_->invalid_html = true;
  base::test::TestFuture<std::u16string, GURL, uint32_t, uint32_t> html;
  clipboard_->ReadHTML(kBuffer, std::nullopt, html.GetCallback());
  EXPECT_TRUE(html.Get<0>().empty());
  EXPECT_TRUE(html.Get<1>().is_empty());
  EXPECT_EQ(html.Get<2>(), 0u);
  EXPECT_EQ(html.Get<3>(), 0u);
  provider_->formats["text/plain"] = "original-data";
  receiver_live_ = false;
  base::test::TestFuture<std::u16string> text;
  clipboard_->ReadText(kBuffer, std::nullopt, text.GetCallback());
  EXPECT_TRUE(text.Get().empty());
  EXPECT_EQ(provider_->calls, 1);
  receiver_live_ = true;
  content::RetireAlloyDocumentState(contents_->GetPrimaryMainFrame());
  base::test::TestFuture<std::u16string> retired;
  clipboard_->ReadText(kBuffer, std::nullopt, retired.GetCallback());
  EXPECT_TRUE(retired.Get().empty());
  EXPECT_EQ(provider_->calls, 1);
}
}  // namespace
}  // namespace alloy::guest::integration
