// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
/* Native C consumer of the experimental profile API. Test-only observations
 * go to stdout, outside CEF roots; no instrumentation is added to the product.
 * A second, blank tab keeps the context alive while the probe closes/reopens
 * the exercised tab. It uses CEF's sharing API to test mode propagation. */
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "include/cef_api_hash.h"
#include "include/capi/alloy/alloy_request_context_capi.h"
#include "include/capi/cef_app_capi.h"
#include "include/capi/cef_browser_capi.h"
#include "include/capi/cef_client_capi.h"
#include "include/capi/cef_dialog_handler_capi.h"
#include "include/capi/cef_preference_capi.h"
#include "include/capi/cef_permission_handler_capi.h"
#include "include/capi/cef_request_handler_capi.h"
#include "include/capi/cef_values_capi.h"

static cef_app_t app;
static cef_browser_process_handler_t process_handler;
static cef_client_t client;
static cef_render_handler_t render;
static cef_life_span_handler_t life;
static cef_display_handler_t display;
static cef_permission_handler_t permissions;
static cef_dialog_handler_t dialogs;
static cef_request_handler_t requests;
static cef_callback_t* held_certificate;
static int hold_next_certificate;
static cef_string_t selected_file, output_file;
static cef_file_dialog_callback_t* held_file;
static int hold_next_file;
static cef_string_t initial_url, blank_url, profile_root;
static cef_request_context_t* context;
static cef_request_context_t* shared_context;
static alloy_network_state_t network_state;
static int browser_count;
static int bootstrap_browser;
static cef_permission_prompt_callback_t* held_permission;
static uint64_t held_prompt_id;
static int hold_next_permission;
static void make_browser(const cef_string_t* url,cef_request_context_t* request_context);
static void start_stateless(void);

static void CEF_CALLBACK add_ref(cef_base_ref_counted_t* self) {}
static int CEF_CALLBACK release_static(cef_base_ref_counted_t* self) { return 0; }
static int CEF_CALLBACK has_one(cef_base_ref_counted_t* self) { return 0; }
static int CEF_CALLBACK has_any(cef_base_ref_counted_t* self) { return 1; }
static void base(cef_base_ref_counted_t* b, size_t size) {
  b->size=size; b->add_ref=add_ref; b->release=release_static;
  b->has_one_ref=has_one; b->has_at_least_one_ref=has_any;
}
static void release_ref(void* object) {
  cef_base_ref_counted_t* b=object;
  if (b) b->release(b);
}
/* CEF's translator consumes a library object's structure reference when it
 * crosses back into the DLL (CppToCRefCounted::Unwrap). Keep owned handles and
 * transfer a separate reference for every refptr argument. Callback refptr
 * arguments and returned objects are owned references too, released below. */
static void* dll_arg(void* object) {
  cef_base_ref_counted_t* b=object;
  if (b) b->add_ref(b);
  return object;
}
static void text(cef_string_t* dest,const char* s) {
  cef_string_utf8_to_utf16(s,strlen(s),dest);
}
static void fail(const char* why) {
  fprintf(stderr,"PROFILE_FAIL %s\n",why); fflush(stderr); abort();
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
static cef_display_handler_t* CEF_CALLBACK get_display(cef_client_t* self) {
  return &display;
}
static cef_permission_handler_t* CEF_CALLBACK get_permissions(cef_client_t* self) {
  return &permissions;
}
static cef_dialog_handler_t* CEF_CALLBACK get_dialogs(cef_client_t* self) {
  return &dialogs;
}
static cef_request_handler_t* CEF_CALLBACK get_requests(cef_client_t* self) {
  return &requests;
}
static int CEF_CALLBACK certificate_error(cef_request_handler_t* self,
    cef_browser_t* browser,cef_errorcode_t error,const cef_string_t* url,
    cef_sslinfo_t* info,cef_callback_t* callback) {
  if (error!=ERR_CERT_AUTHORITY_INVALID) fail("unexpected fixture certificate error");
  printf("PROFILE_EVENT {\"event\":\"certificate\",\"browser\":%d,\"held\":%d,\"error\":%d}\n",
      browser->get_identifier(browser),hold_next_certificate,error);
  if (hold_next_certificate) {
    if (held_certificate) fail("overlapping certificate decisions");
    held_certificate=callback;hold_next_certificate=0;
  } else {
    callback->cont(callback);
    release_ref(callback);
  }
  release_ref(info);release_ref(browser);
  return 1;
}
static void select_fixture_file(cef_file_dialog_callback_t* callback,const cef_string_t* path) {
  cef_string_list_t files=cef_string_list_alloc();
  cef_string_list_append(files,path);
  callback->cont(callback,files);
  cef_string_list_free(files);
  release_ref(callback);
}
static int CEF_CALLBACK file_dialog(cef_dialog_handler_t* self,
    cef_browser_t* browser,cef_file_dialog_mode_t mode,const cef_string_t* title,
    const cef_string_t* path,cef_string_list_t filters,cef_string_list_t extensions,
    cef_string_list_t descriptions,cef_file_dialog_callback_t* callback) {
  if (mode!=FILE_DIALOG_OPEN && mode!=FILE_DIALOG_SAVE) fail("unexpected file dialog mode");
  printf("PROFILE_EVENT {\"event\":\"file-picker\",\"browser\":%d,\"held\":%d,\"save\":%d}\n",
      browser->get_identifier(browser),hold_next_file,mode==FILE_DIALOG_SAVE);
  if (hold_next_file) {
    if (held_file) fail("overlapping file pickers");
    held_file=callback;hold_next_file=0;
  } else select_fixture_file(callback,mode==FILE_DIALOG_SAVE?&output_file:&selected_file);
  release_ref(browser);
  return 1;
}
static int CEF_CALLBACK permission_prompt(cef_permission_handler_t* self,
    cef_browser_t* browser,uint64_t prompt_id,const cef_string_t* origin,
    uint32_t requested,cef_permission_prompt_callback_t* callback) {
  printf("PROFILE_EVENT {\"event\":\"permission\",\"browser\":%d,"
      "\"prompt\":%llu,\"permissions\":%u,\"held\":%d}\n",browser->get_identifier(browser),
      (unsigned long long)prompt_id,requested,hold_next_permission);
  if (requested!=CEF_PERMISSION_TYPE_GEOLOCATION) fail("unexpected permission prompt");
  if (hold_next_permission) {
    if (held_permission) fail("overlapping held permission");
    held_permission=callback; held_prompt_id=prompt_id; hold_next_permission=0;
  } else {
    callback->cont(callback,CEF_PERMISSION_RESULT_ACCEPT);
    release_ref(callback);
  }
  release_ref(browser);
  return 1;
}
static void CEF_CALLBACK permission_dismissed(cef_permission_handler_t* self,
    cef_browser_t* browser,uint64_t prompt_id,cef_permission_request_result_t result) {
  printf("PROFILE_EVENT {\"event\":\"permission-dismissed\",\"browser\":%d,"
      "\"prompt\":%llu,\"result\":%d}\n",browser->get_identifier(browser),
      (unsigned long long)prompt_id,result);
  release_ref(browser);
}
static int CEF_CALLBACK media_permission(cef_permission_handler_t* self,
    cef_browser_t* browser,cef_frame_t* frame,const cef_string_t* origin,
    uint32_t requested,cef_media_access_callback_t* callback) {
  if (requested!=CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE)
    fail("unexpected media permission request");
  printf("PROFILE_EVENT {\"event\":\"media-permission\",\"browser\":%d,\"permissions\":%u}\n",
      browser->get_identifier(browser),requested);
  callback->cont(callback,requested);
  release_ref(callback); release_ref(frame); release_ref(browser);
  return 1;
}
static void CEF_CALLBACK register_preferences(cef_browser_process_handler_t* self,
    cef_preferences_type_t type,cef_preference_registrar_t* registrar) {
  if (type!=CEF_PREFERENCES_TYPE_REQUEST_CONTEXT) return;
  cef_string_t key={0}, initial={0};
  text(&key,"alloy.profile_probe.parent"); text(&initial,"registered-default");
  cef_value_t* value=cef_value_create();
  value->set_string(value,&initial);
  if (!registrar->add_preference(registrar,&key,dll_arg(value)))
    fail("custom preference registration failed");
  release_ref(value); cef_string_clear(&key); cef_string_clear(&initial);
}
static void CEF_CALLBACK view(cef_render_handler_t* self,cef_browser_t* browser,
    cef_rect_t* rect) {
  rect->x=0; rect->y=0; rect->width=1000; rect->height=700;
  release_ref(browser);
}
static void CEF_CALLBACK paint(cef_render_handler_t* self,cef_browser_t* browser,
    cef_paint_element_type_t type,size_t n,const cef_rect_t* rects,
    const void* pixels,int width,int height) { release_ref(browser); }
static int parent_preference_isolated(cef_request_context_t* actual) {
  cef_string_t key={0}; text(&key,"alloy.profile_probe.parent");
  cef_value_t* value=actual->base.get_preference(&actual->base,&key);
  cef_string_clear(&key);
  if (!value || value->get_type(value)!=VTYPE_STRING)
    fail("child preference control missing");
  cef_string_userfree_t result=value->get_string(value);
  cef_string_utf8_t utf8={0};
  if (result) cef_string_utf16_to_utf8(result->str,result->length,&utf8);
  int isolated=utf8.str && !strcmp(utf8.str,"registered-default");
  cef_string_utf8_clear(&utf8);
  if (result) cef_string_userfree_free(result);
  release_ref(value);
  return isolated!=0;
}
static void CEF_CALLBACK created(cef_life_span_handler_t* self,
    cef_browser_t* browser) {
  ++browser_count;
  cef_browser_host_t* host=browser->get_host(browser);
  cef_request_context_t* actual=host->get_request_context(host);
  if (!context) {
    if (bootstrap_browser || !actual->is_global(actual)) fail("unexpected bootstrap context");
    bootstrap_browser=browser->get_identifier(browser);
    cef_string_userfree_t cache=actual->get_cache_path(actual);
    int persistent=cache && cache->length>0;
    if (cache) cef_string_userfree_free(cache);
    if (!persistent) fail("normal OSR parity context is not persistent");
    printf("PROFILE_EVENT {\"event\":\"bootstrap-created\",\"browser\":%d,\"persistent_context\":%d}\n",bootstrap_browser,persistent);
    release_ref(actual); release_ref(host); release_ref(browser);
    return;
  }
  int shares=actual->is_sharing_with(actual,dll_arg(context));
  cef_string_userfree_t cache=actual->get_cache_path(actual);
  int no_cache_path=!cache || cache->length==0;
  int isolated=parent_preference_isolated(actual);
  if (cache) cef_string_userfree_free(cache);
  printf("PROFILE_EVENT {\"event\":\"created\",\"browser\":%d,"
      "\"shares_context\":%d,\"empty_cache_path\":%d,"
      "\"parent_preference_isolated\":%d,\"can_back\":%d,"
      "\"can_forward\":%d}\n",browser->get_identifier(browser),shares,
      no_cache_path,isolated,browser->can_go_back(browser),browser->can_go_forward(browser));
  release_ref(actual); release_ref(host);
  if (!shares || !no_cache_path || !isolated) fail("browser lost stateless context");
  release_ref(browser);
}
static void CEF_CALLBACK closed(cef_life_span_handler_t* self,
    cef_browser_t* browser) {
  printf("PROFILE_EVENT {\"event\":\"%s\",\"browser\":%d}\n",
       browser->get_identifier(browser)==bootstrap_browser ? "bootstrap-closed" : "closed",
       browser->get_identifier(browser));
  if (--browser_count==0) cef_quit_message_loop();
  release_ref(browser);
}
static void CEF_CALLBACK title_changed(cef_display_handler_t* self,
    cef_browser_t* browser,const cef_string_t* title) {
  static const char prefix[]="ALLOY_PROFILE_PROBE:";
  cef_string_utf8_t utf8={0};
  cef_string_utf16_to_utf8(title->str,title->length,&utf8);
  if (utf8.str && !strncmp(utf8.str,prefix,sizeof(prefix)-1)) {
    const char* token=utf8.str+sizeof(prefix)-1;
    if (strlen(token) && strspn(token,
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_")==strlen(token)) {
      printf("PROFILE_EVENT {\"event\":\"navigation\",\"token\":\"%s\","
          "\"browser\":%d,\"can_back\":%d,\"can_forward\":%d}\n",
          token,browser->get_identifier(browser),browser->can_go_back(browser),
           browser->can_go_forward(browser));
       if (!strcmp(token,"start-stateless")) {
         if (context || browser->get_identifier(browser)!=bootstrap_browser)
           fail("invalid stateless start control");
         start_stateless();
       }
       if (!strcmp(token,"dispose-stateless")) {
         if (!context || browser_count!=1 ||
             browser->get_identifier(browser)!=bootstrap_browser)
           fail("stateless disposal before its browsers closed");
         release_ref(shared_context); shared_context=NULL;
         release_ref(context); context=NULL;
         printf("PROFILE_EVENT {\"event\":\"stateless-released\"}\n");
       }
       if (!strcmp(token,"zoom-set")) {
         cef_browser_host_t* host=browser->get_host(browser);
         host->set_zoom_level(host,1.0);
         printf("PROFILE_EVENT {\"event\":\"zoom\",\"browser\":%d,\"level\":%.8f}\n",
             browser->get_identifier(browser),host->get_zoom_level(host));
         release_ref(host);
       }
        if (!strcmp(token,"hold-file")) {
         hold_next_file=1;
         printf("PROFILE_EVENT {\"event\":\"file-hold-ready\"}\n");
        }
        if (!strcmp(token,"hold-certificate")) {
          hold_next_certificate=1;
          printf("PROFILE_EVENT {\"event\":\"certificate-hold-ready\"}\n");
        }
        if (!strcmp(token,"release-held-certificate")) {
          if (!held_certificate) fail("no held certificate decision");
          held_certificate->cont(held_certificate);
          release_ref(held_certificate);held_certificate=NULL;
          printf("PROFILE_EVENT {\"event\":\"certificate-released\"}\n");
        }
       if (!strcmp(token,"release-held-file")) {
         if (!held_file) fail("no held file picker");
         select_fixture_file(held_file,&selected_file);held_file=NULL;
         printf("PROFILE_EVENT {\"event\":\"file-released\"}\n");
       }
       if (!strncmp(token,"open-tab-",9)) make_browser(&blank_url,shared_context);
       if (!strcmp(token,"hold-permission")) {
         hold_next_permission=1;
         printf("PROFILE_EVENT {\"event\":\"permission-hold-ready\"}\n");
       }
       if (!strcmp(token,"release-held-permission")) {
         if (!held_permission) fail("no held permission to release");
         held_permission->cont(held_permission,CEF_PERMISSION_RESULT_ACCEPT);
         release_ref(held_permission); held_permission=NULL;
         printf("PROFILE_EVENT {\"event\":\"permission-released\",\"prompt\":%llu}\n",
             (unsigned long long)held_prompt_id);
       }
    }
  }
  cef_string_utf8_clear(&utf8);
  release_ref(browser);
}
static void make_browser(const cef_string_t* url,cef_request_context_t* request_context) {
  cef_window_info_t window={0}; window.size=sizeof(window);
  window.windowless_rendering_enabled=1;
  window.runtime_style=CEF_RUNTIME_STYLE_ALLOY;
  cef_browser_settings_t settings={0}; settings.size=sizeof(settings);
  if (!cef_browser_host_create_browser(&window,&client,url,&settings,NULL,
                                      dll_arg(request_context))) fail("browser creation refused");
}
static void start_stateless(void) {
  printf("PROFILE_EVENT {\"event\":\"stateless-start\"}\n");
  if (alloy_create_stateless_request_context((alloy_network_state_t)-1,NULL))
    fail("invalid network option accepted");
  context=alloy_create_stateless_request_context(network_state,NULL);
  if (!context || context->is_global(context)) fail("stateless creation failed");
  shared_context=cef_request_context_cef_create_context_shared(dll_arg(context),NULL);
  if (!shared_context || !shared_context->is_sharing_with(shared_context,dll_arg(context)))
    fail("shared context creation failed");
  /* Request-context initialization can be asynchronous. The browser creation
   * API waits for it; inspect effective preferences when browsers exist. */
  make_browser(&initial_url,context);
  make_browser(&blank_url,shared_context);
}
static void CEF_CALLBACK initialized(cef_browser_process_handler_t* self) {
  /* This explicit persistent parent is only fixture setup. Observe its native
   * startup/store completion and inventory it before starting the context. */
  cef_request_context_t* parent=cef_request_context_get_global_context();
  cef_string_t key={0}, sentinel={0}, error={0};
  text(&key,"alloy.profile_probe.parent"); text(&sentinel,"alloy-parent-sentinel");
  cef_value_t* value=cef_value_create();
  value->set_string(value,&sentinel);
  if (!parent->base.set_preference(&parent->base,&key,dll_arg(value),&error))
    fail("cannot seed parent preference control");
  release_ref(value);
  make_browser(&blank_url,parent);
  cef_string_clear(&key); cef_string_clear(&sentinel); cef_string_clear(&error);
  release_ref(parent);
}
int main(int argc,char** argv) {
  setvbuf(stdout,NULL,_IOLBF,0);
  if (!cef_api_hash(CEF_API_VERSION,0)) return 8;
  cef_main_args_t args={argc,argv};
  int code=cef_execute_process(&args,NULL,NULL);
  if (code>=0) return code;
  printf("PROFILE_EVENT {\"event\":\"process\",\"pid\":%ld}\n",(long)getpid());
  const char* url=getenv("ALLOY_PROFILE_URL");
  const char* root=getenv("ALLOY_PROFILE_ROOT");
  const char* network=getenv("ALLOY_PROFILE_NETWORK");
  const char* file=getenv("ALLOY_PROFILE_SELECTED_FILE");
  const char* output=getenv("ALLOY_PROFILE_OUTPUT_FILE");
  if (!url || !root || !network || !file || !output) return 9;
  if (!strcmp(network,"keep")) network_state=ALLOY_NETWORK_STATE_KEEP;
  else if (!strcmp(network,"cut")) network_state=ALLOY_NETWORK_STATE_CUT;
  else return 10;
  /* Own every callback input before Chromium's process-title initialization
   * moves environ and invalidates borrowed getenv storage. */
  text(&initial_url,url); text(&blank_url,"about:blank"); text(&profile_root,root);
  text(&selected_file,file);
  text(&output_file,output);
  base(&app.base,sizeof(app)); base(&process_handler.base,sizeof(process_handler));
  base(&client.base,sizeof(client)); base(&render.base,sizeof(render));
  base(&life.base,sizeof(life)); base(&display.base,sizeof(display));
  base(&permissions.base,sizeof(permissions));
  base(&requests.base,sizeof(requests));requests.on_certificate_error=certificate_error;
  base(&dialogs.base,sizeof(dialogs));dialogs.on_file_dialog=file_dialog;
  app.get_browser_process_handler=get_process;
  process_handler.on_context_initialized=initialized;
  process_handler.on_register_custom_preferences=register_preferences;
  client.get_render_handler=get_render; client.get_life_span_handler=get_life;
  client.get_display_handler=get_display;
  client.get_permission_handler=get_permissions;
  client.get_dialog_handler=get_dialogs;
  client.get_request_handler=get_requests;
  permissions.on_show_permission_prompt=permission_prompt;
  permissions.on_dismiss_permission_prompt=permission_dismissed;
  permissions.on_request_media_access_permission=media_permission;
  render.get_view_rect=view; render.on_paint=paint;
  life.on_after_created=created; life.on_before_close=closed;
  display.on_title_change=title_changed;
  cef_settings_t settings={0}; settings.size=sizeof(settings);
  settings.windowless_rendering_enabled=1;
  cef_string_copy(profile_root.str,profile_root.length,&settings.cache_path);
  cef_string_copy(profile_root.str,profile_root.length,&settings.root_cache_path);
  if (!cef_initialize(&args,&settings,&app,NULL)) return cef_get_exit_code();
  cef_run_message_loop();
  if (held_permission) fail("held permission not exercised");
  if (held_file) fail("held file picker not exercised");
  if (held_certificate) fail("held certificate decision not exercised");
  release_ref(shared_context); release_ref(context);
  cef_shutdown();
  cef_string_clear(&settings.cache_path); cef_string_clear(&settings.root_cache_path);
  cef_string_clear(&initial_url); cef_string_clear(&blank_url);
  cef_string_clear(&profile_root);
  cef_string_clear(&selected_file);
  cef_string_clear(&output_file);
  return 0;
}
