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
#include "profile_control.h"
#include "include/cef_api_hash.h"
#include "include/capi/alloy/alloy_request_context_capi.h"
#include "include/capi/cef_app_capi.h"
#include "include/capi/cef_browser_capi.h"
#include "include/capi/cef_client_capi.h"
#include "include/capi/cef_command_line_capi.h"
#include "include/capi/cef_dialog_handler_capi.h"
#include "include/capi/cef_preference_capi.h"
#include "include/capi/cef_permission_handler_capi.h"
#include "include/capi/cef_request_handler_capi.h"
#include "include/capi/cef_values_capi.h"
#define UNIT_TEST 1
#include "include/capi/test/cef_test_helpers_capi.h"

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
static cef_string_t initial_url, blank_url, profile_root, embedded_scope;
static cef_request_context_t* context;
static cef_request_context_t* shared_context;
static cef_request_context_t* peer_context;
static int peer_browser;
static int peer_memory_only;
static alloy_network_state_t network_state;
static int browser_count;
static int bootstrap_browser;
static int no_disk_bootstrap;
static int persistent_only;
static int memory_only;
static int control_setup_started, control_setup_complete;
static int inject_no_disk_write, no_disk_write_injected;
static cef_completion_callback_t control_persistence_callback;
static int primary_scope_configured, peer_scope_configured;
static cef_permission_prompt_callback_t* held_permission;
static uint64_t held_prompt_id;
static int hold_next_permission;
static profile_control_transport control_transport;
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
static void remove_no_disk_transport(cef_command_line_t* command_line,
                                    const char* stage) {
  if (no_disk_bootstrap) {
    cef_string_t key={0};text(&key,"alloy-no-disk");
    if (command_line->is_read_only(command_line)) fail("read-only launch command line");
    command_line->remove_switch(command_line,&key);
    if (command_line->has_switch(command_line,&key)) fail("launch switch not removed");
    cef_string_clear(&key);
    printf("PROFILE_EVENT {\"event\":\"no-disk-command-line-test\",\"stage\":\"%s\"}\n",stage);
  }
  release_ref(command_line);
}
static void CEF_CALLBACK process_command_line(cef_app_t* self,
    const cef_string_t* process_type,cef_command_line_t* command_line) {
  remove_no_disk_transport(command_line,"bootstrap");
}
static void CEF_CALLBACK child_command_line(cef_browser_process_handler_t* self,
    cef_command_line_t* command_line) {
  remove_no_disk_transport(command_line,"child");
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
  int peer=peer_context && actual->is_sharing_with(actual,dll_arg(peer_context));
  int* scope_configured=peer?&peer_scope_configured:&primary_scope_configured;
  if (embedded_scope.length && !*scope_configured) {
    cef_string_t invalid_scope={0};text(&invalid_scope,"invalid-embedded-scope");
    if (alloy_set_embedded_state_scope(NULL,&embedded_scope) ||
        alloy_set_embedded_state_scope(dll_arg(actual),&invalid_scope))
      fail("invalid embedded scope input accepted");
    cef_string_clear(&invalid_scope);
    if (!alloy_set_embedded_state_scope(dll_arg(actual),&embedded_scope))
      fail("embedded scope selection before document admission refused");
    *scope_configured=1;
    printf("PROFILE_EVENT {\"event\":\"embedded-scope-configured\",\"browser\":%d}\n",
        browser->get_identifier(browser));
  } else if (embedded_scope.length) {
    printf("PROFILE_EVENT {\"event\":\"embedded-scope-reused\",\"browser\":%d}\n",
        browser->get_identifier(browser));
  }
  if (peer) {
    if (peer_browser) fail("multiple independent peer browsers");
    peer_browser=browser->get_identifier(browser);
    if (actual->is_sharing_with(actual,dll_arg(context)))
      fail("independent context shares with primary");
  }
  int shares=actual->is_sharing_with(actual,dll_arg(peer?peer_context:context));
  cef_string_userfree_t cache=actual->get_cache_path(actual);
  int no_cache_path=!cache || cache->length==0;
  int isolated=persistent_only?0:parent_preference_isolated(actual);
  if (cache) cef_string_userfree_free(cache);
  printf("PROFILE_EVENT {\"event\":\"created\",\"browser\":%d,"
      "\"shares_context\":%d,\"empty_cache_path\":%d,"
      "\"parent_preference_isolated\":%d,\"can_back\":%d,"
      "\"can_forward\":%d,\"slot\":\"%s\",\"memory\":%d}\n",browser->get_identifier(browser),shares,
      no_cache_path,isolated,browser->can_go_back(browser),browser->can_go_forward(browser),
      peer?"peer":"primary",peer?peer_memory_only:memory_only);
  release_ref(actual); release_ref(host);
  if (!shares || (!persistent_only && (!no_cache_path || !isolated)) ||
      (persistent_only && no_cache_path)) fail("browser lost its native context mode");
  if (inject_no_disk_write && !no_disk_write_injected && !peer) {
    if (!memory_only || !control_setup_complete || no_disk_bootstrap)
      fail("invalid post-barrier no-disk negative control");
    cef_string_utf8_t root={0};
    cef_string_utf16_to_utf8(profile_root.str,profile_root.length,&root);
    char path[4096];
    if (!root.str || snprintf(path,sizeof(path),"%s/no-disk-negative-control",root.str)>=(int)sizeof(path))
      fail("negative control path invalid");
    FILE* file=fopen(path,"wx");
    if (!file || fputs("intentional no-disk-context fault injection\n",file)<0 || fclose(file))
      fail("negative control write failed");
    cef_string_utf8_clear(&root);no_disk_write_injected=1;
    printf("PROFILE_EVENT {\"event\":\"no-disk-write-injected\",\"browser\":%d,\"after_native_barrier\":1}\n",
        browser->get_identifier(browser));
  }
  release_ref(browser);
}
static void CEF_CALLBACK closed(cef_life_span_handler_t* self,
    cef_browser_t* browser) {
  printf("PROFILE_EVENT {\"event\":\"%s\",\"browser\":%d}\n",
       browser->get_identifier(browser)==bootstrap_browser ? "bootstrap-closed" : "closed",
       browser->get_identifier(browser));
  if (browser->get_identifier(browser)==peer_browser) peer_browser=0;
  if (--browser_count==0) cef_quit_message_loop();
  release_ref(browser);
}
static void dispatch_control(void* opaque,
    const profile_control_request* command) {
  cef_browser_t* browser=opaque;
  const char* token=command->action;
    if (strlen(token) && strspn(token,
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_")==strlen(token)) {
      printf("PROFILE_EVENT {\"event\":\"navigation\",\"token\":\"%s\","
          "\"browser\":%d,\"can_back\":%d,\"can_forward\":%d,\"request_id\":\"%s\"}\n",
          token,browser->get_identifier(browser),browser->can_go_back(browser),
           browser->can_go_forward(browser),command->id);
        if (!strcmp(token,"prepare-control")) {
          if (context || control_setup_started || browser->get_identifier(browser)!=bootstrap_browser)
            fail("invalid persistent-control setup request");
          control_setup_started=1;
          if (!cef_flush_browser_persistent_state_for_tests(dll_arg(browser),dll_arg(&control_persistence_callback)))
            fail("native persistent-control fence refused");
        } else if (!strcmp(token,"start-stateless")) {
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
        if (!strcmp(token,"memory-peer") || !strcmp(token,"memory-peer-stateless")) {
          if (!memory_only || !context || peer_context)
            fail("invalid independent context creation");
           peer_memory_only=!strcmp(token,"memory-peer");
           peer_scope_configured=0;
          peer_context=peer_memory_only?alloy_create_memory_request_context(NULL):
              alloy_create_stateless_request_context(ALLOY_NETWORK_STATE_CUT,NULL);
          if (!peer_context || peer_context->is_sharing_with(peer_context,dll_arg(context)))
            fail("independent context is not isolated");
          make_browser(&blank_url,peer_context);
        }
        if (!strcmp(token,"memory-release-peer")) {
          if (!memory_only || !peer_context || peer_browser)
            fail("independent context released before its browser closed");
          release_ref(peer_context);peer_context=NULL;
          printf("PROFILE_EVENT {\"event\":\"memory-peer-released\"}\n");
        }
        if (!strcmp(token,"memory-restart")) {
          if (!memory_only || context || browser->get_identifier(browser)!=bootstrap_browser)
            fail("memory recreation before primary context disposal");
          start_stateless();
        }
        if (!strcmp(token,"zoom-set")) {
         cef_browser_host_t* host=browser->get_host(browser);
         host->set_zoom_level(host,1.0);
         printf("PROFILE_EVENT {\"event\":\"zoom\",\"browser\":%d,\"level\":%.8f}\n",
             browser->get_identifier(browser),host->get_zoom_level(host));
          release_ref(host);
        }
        if (!strcmp(token,"embedded-scope-late")) {
          cef_browser_host_t* host=browser->get_host(browser);
          cef_request_context_t* actual=host->get_request_context(host);
          if (!embedded_scope.length ||
              alloy_set_embedded_state_scope(dll_arg(actual),&embedded_scope))
            fail("late embedded scope change was accepted");
          printf("PROFILE_EVENT {\"event\":\"embedded-scope-late-refused\"}\n");
          release_ref(actual);release_ref(host);
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
static void control_reply(void* opaque,
    const profile_control_request* command,int replayed) {
  (void)opaque;
  printf("PROFILE_EVENT {\"event\":\"control-reply\",\"request_id\":\"%s\","
      "\"token\":\"%s\",\"browser\":%d,\"phase\":\"%s\","
      "\"replayed\":%d,\"executions\":%u}\n",command->id,command->action,
      command->browser,command->complete?"dispatched":"pending",replayed,
      command->executions);
  fflush(stdout);
}
static void CEF_CALLBACK title_changed(cef_display_handler_t* self,
    cef_browser_t* browser,const cef_string_t* title) {
  cef_string_utf8_t utf8={0};
  cef_string_utf16_to_utf8(title->str,title->length,&utf8);
  int result=profile_control_receive(&control_transport,browser->get_identifier(browser),
      utf8.str,dispatch_control,control_reply,browser);
  if (result<0) fail("invalid or conflicting native control request");
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
  if (!no_disk_bootstrap && !control_setup_complete)
    fail("no-disk context creation before native control persistence completion");
  printf("PROFILE_EVENT {\"event\":\"%s-start\"}\n",memory_only?"memory":"stateless");
  primary_scope_configured=0;
  if (!memory_only && alloy_create_stateless_request_context((alloy_network_state_t)-1,NULL))
    fail("invalid network option accepted");
  context=memory_only?alloy_create_memory_request_context(NULL):
      alloy_create_stateless_request_context(network_state,NULL);
  if (!context || context->is_global(context)) fail("isolated context creation failed");
  shared_context=cef_request_context_cef_create_context_shared(dll_arg(context),NULL);
  if (!shared_context || !shared_context->is_sharing_with(shared_context,dll_arg(context)))
    fail("shared context creation failed");
  /* Request-context initialization can be asynchronous. The browser creation
   * API waits for it; inspect effective preferences when browsers exist. */
  make_browser(&initial_url,context);
  make_browser(&blank_url,shared_context);
}
static void CEF_CALLBACK control_persistence_complete(cef_completion_callback_t* self) {
  if (!control_setup_started || control_setup_complete || context || !bootstrap_browser)
    fail("invalid native control-persistence completion");
  control_setup_complete=1;
  printf("PROFILE_EVENT {\"event\":\"parent-persistence-complete\",\"browser\":%d}\n",bootstrap_browser);
}
static void CEF_CALLBACK initialized(cef_browser_process_handler_t* self) {
  if (persistent_only) {
    context=cef_request_context_get_global_context();
    if (!context || !context->is_global(context)) fail("ordinary global context missing");
    shared_context=cef_request_context_cef_create_context_shared(dll_arg(context),NULL);
    if (!shared_context || !shared_context->is_sharing_with(shared_context,dll_arg(context)))
      fail("ordinary sharing alias missing");
    printf("PROFILE_EVENT {\"event\":\"ordinary-persistent-start\"}\n");
    make_browser(&initial_url,context);
    make_browser(&blank_url,shared_context);
    return;
  }
  if (no_disk_bootstrap) {
    start_stateless();
    return;
  }
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
  const char* scope=getenv("ALLOY_PROFILE_EMBEDDED_SCOPE");
  no_disk_bootstrap=getenv("ALLOY_PROFILE_NO_DISK")!=NULL;
  persistent_only=getenv("ALLOY_PROFILE_PERSISTENT")!=NULL;
  if (persistent_only && no_disk_bootstrap) fail("conflicting native context modes");
  memory_only=getenv("ALLOY_PROFILE_MEMORY")!=NULL;
  inject_no_disk_write=getenv("ALLOY_PROFILE_INJECT_NO_DISK_WRITE")!=NULL;
  if (!url || !root || !network || !file || !output) return 9;
  if (!strcmp(network,"keep")) network_state=ALLOY_NETWORK_STATE_KEEP;
  else if (!strcmp(network,"cut")) network_state=ALLOY_NETWORK_STATE_CUT;
  else return 10;
  /* Own every callback input before Chromium's process-title initialization
   * moves environ and invalidates borrowed getenv storage. */
  text(&initial_url,url); text(&blank_url,"about:blank"); text(&profile_root,root);
  text(&selected_file,file);
  text(&output_file,output);
  if (scope) text(&embedded_scope,scope);
  base(&app.base,sizeof(app)); base(&process_handler.base,sizeof(process_handler));
  base(&client.base,sizeof(client)); base(&render.base,sizeof(render));
  base(&life.base,sizeof(life)); base(&display.base,sizeof(display));
  base(&permissions.base,sizeof(permissions));
  base(&requests.base,sizeof(requests));requests.on_certificate_error=certificate_error;
  base(&control_persistence_callback.base,sizeof(control_persistence_callback));
  control_persistence_callback.on_complete=control_persistence_complete;
  base(&dialogs.base,sizeof(dialogs));dialogs.on_file_dialog=file_dialog;
  app.get_browser_process_handler=get_process;
  app.on_before_command_line_processing=process_command_line;
  process_handler.on_context_initialized=initialized;
  process_handler.on_before_child_process_launch=child_command_line;
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
  if (!no_disk_bootstrap)
    cef_string_copy(profile_root.str,profile_root.length,&settings.cache_path);
  cef_string_copy(profile_root.str,profile_root.length,&settings.root_cache_path);
  if (!cef_initialize(&args,&settings,&app,NULL)) return cef_get_exit_code();
  cef_run_message_loop();
  if (held_permission) fail("held permission not exercised");
  if (held_file) fail("held file picker not exercised");
  if (held_certificate) fail("held certificate decision not exercised");
  release_ref(peer_context); release_ref(shared_context); release_ref(context);
  cef_shutdown();
  profile_control_destroy(&control_transport);
  cef_string_clear(&settings.cache_path); cef_string_clear(&settings.root_cache_path);
  cef_string_clear(&initial_url); cef_string_clear(&blank_url);
  cef_string_clear(&profile_root);
  cef_string_clear(&embedded_scope);
  cef_string_clear(&selected_file);
  cef_string_clear(&output_file);
  return 0;
}
