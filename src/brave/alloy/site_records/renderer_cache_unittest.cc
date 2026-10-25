// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "gin/test/v8_test.h"
#include "v8/include/v8-alloy-cache-control.h"
#include "v8/include/v8.h"

namespace alloy::site_records {
namespace {

class RendererCompilationCacheTest : public gin::V8Test {};

TEST_F(RendererCompilationCacheTest, RepeatedDocumentScriptsCompileFreshAfterOptOut) {
  auto* isolate = instance_->isolate();
  v8::HandleScope handles(isolate);
  // Gin's fixture defaults to strict mode. V8's script cache deliberately
  // accepts sloppy scripts only; exercise it and restore the fixture setting.
  v8::V8::SetFlagsFromString("--no-use-strict");
  base::ScopedClosureRunner restore(base::BindOnce([] {
    v8::V8::SetFlagsFromString("--use-strict");
  }));
  auto compile = [&] {
    auto document = v8::Context::New(isolate);
    v8::Context::Scope entered(document);
    v8::ScriptOrigin origin(v8::String::NewFromUtf8Literal(
        isolate, "https://alloy.test/document-script.js"));
    v8::ScriptCompiler::Source source(v8::String::NewFromUtf8Literal(
        isolate, "function value() { return 42; } value();"), origin);
    return v8::ScriptCompiler::CompileUnboundScript(isolate, &source).ToLocalChecked();
  };
  auto first = compile();
  auto second = compile();
  ASSERT_EQ(first->GetId(), second->GetId()) << "positive cache control did not hit";
  v8::AlloyDisableCompilationCaches(isolate);
  auto third = compile();
  auto fourth = compile();
  EXPECT_NE(first->GetId(), third->GetId());
  EXPECT_NE(third->GetId(), fourth->GetId());
}

}  // namespace
}  // namespace alloy::site_records
