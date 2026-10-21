// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
/* Observe CEF's native AX spelling markers for a real edited textarea.
 * No product instrumentation or renderer test-only features. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/cef_api_hash.h"
#include "include/capi/cef_app_capi.h"
#include "include/capi/cef_browser_capi.h"
#include "include/capi/cef_client_capi.h"
#include "include/capi/cef_parser_capi.h"

static cef_app_t app;
static cef_browser_process_handler_t process_handler;
static cef_client_t client;
static cef_render_handler_t render;
static cef_accessibility_handler_t accessibility;
static cef_life_span_handler_t life;
static cef_string_t initial_url;

static void CEF_CALLBACK add_ref(cef_base_ref_counted_t* self) {}
static int CEF_CALLBACK release(cef_base_ref_counted_t* self) { return 0; }
static int CEF_CALLBACK has_one(cef_base_ref_counted_t* self) { return 0; }
static int CEF_CALLBACK has_any(cef_base_ref_counted_t* self) { return 1; }
static void base(cef_base_ref_counted_t* b, size_t size) {
  b->size=size; b->add_ref=add_ref; b->release=release;
  b->has_one_ref=has_one; b->has_at_least_one_ref=has_any;
}
static void text(cef_string_t* dest,const char* s) {
  cef_string_utf8_to_utf16(s,strlen(s),dest);
}
static cef_browser_process_handler_t* CEF_CALLBACK get_process(cef_app_t* self) {
  return &process_handler;
}
static cef_render_handler_t* CEF_CALLBACK get_render(cef_client_t* self) {
  return &render;
}
static cef_life_span_handler_t* CEF_CALLBACK get_life(cef_client_t* self) {
  return &life;
}
static cef_accessibility_handler_t* CEF_CALLBACK get_accessibility(cef_render_handler_t* self) {
  return &accessibility;
}
static void CEF_CALLBACK view(cef_render_handler_t* self,cef_browser_t* browser,cef_rect_t* rect) {
  rect->x=0; rect->y=0; rect->width=1000; rect->height=700;
}
static void CEF_CALLBACK paint(cef_render_handler_t* self,cef_browser_t* browser,
    cef_paint_element_type_t type,size_t n,const cef_rect_t* rects,const void* pixels,int width,int height) {}
static void CEF_CALLBACK tree(cef_accessibility_handler_t* self,cef_value_t* value) {
  cef_string_userfree_t json=cef_write_json(value,JSON_WRITER_DEFAULT);
  if (!json) return;
  cef_string_utf8_t utf8={0};
  cef_string_utf16_to_utf8(json->str,json->length,&utf8);
  printf("AX %s\n",utf8.str); fflush(stdout);
  cef_string_utf8_clear(&utf8); cef_string_userfree_free(json);
}
static void CEF_CALLBACK location(cef_accessibility_handler_t* self,cef_value_t* value) {}
static void CEF_CALLBACK created(cef_life_span_handler_t* self,cef_browser_t* browser) {
  cef_browser_host_t* host=browser->get_host(browser);
  host->set_accessibility_state(host,STATE_ENABLED);
  host->set_focus(host,1);
  host->base.release(&host->base);
}
static void CEF_CALLBACK closed(cef_life_span_handler_t* self,cef_browser_t* browser) {
  cef_quit_message_loop();
}
static void CEF_CALLBACK initialized(cef_browser_process_handler_t* self) {
  cef_window_info_t window={0}; window.size=sizeof(window);
  window.windowless_rendering_enabled=1;
  window.runtime_style=CEF_RUNTIME_STYLE_ALLOY;
  cef_browser_settings_t settings={0}; settings.size=sizeof(settings);
  if (!cef_browser_host_create_browser(&window,&client,&initial_url,&settings,NULL,NULL)) abort();
}
int main(int argc,char** argv) {
  setvbuf(stdout,NULL,_IOLBF,0);
  if (!cef_api_hash(CEF_API_VERSION,0)) return 8;
  cef_main_args_t args={argc,argv};
  int code=cef_execute_process(&args,NULL,NULL);
  if (code>=0) return code;
  const char* url=getenv("ALLOY_SPELL_URL");
  const char* profile=getenv("ALLOY_SPELL_PROFILE");
  if (!url || !profile) return 9;
  // Own callback inputs before CEF startup: Chromium's Linux process-title
  // initialization moves environ strings and overwrites their old storage.
  // A retained getenv pointer is not valid in OnContextInitialized.
  text(&initial_url,url);
  base(&app.base,sizeof(app)); base(&process_handler.base,sizeof(process_handler));
  base(&client.base,sizeof(client)); base(&render.base,sizeof(render));
  base(&accessibility.base,sizeof(accessibility)); base(&life.base,sizeof(life));
  app.get_browser_process_handler=get_process;
  process_handler.on_context_initialized=initialized;
  client.get_render_handler=get_render; client.get_life_span_handler=get_life;
  render.get_accessibility_handler=get_accessibility; render.get_view_rect=view; render.on_paint=paint;
  accessibility.on_accessibility_tree_change=tree;
  accessibility.on_accessibility_location_change=location;
  life.on_after_created=created; life.on_before_close=closed;
  cef_settings_t settings={0}; settings.size=sizeof(settings);
  settings.windowless_rendering_enabled=1;
  text(&settings.cache_path,profile); text(&settings.root_cache_path,profile);
  if (!cef_initialize(&args,&settings,&app,NULL)) {
    cef_string_clear(&initial_url);
    return cef_get_exit_code();
  }
  cef_run_message_loop();
  cef_shutdown();
  cef_string_clear(&initial_url);
  return 0;
}
