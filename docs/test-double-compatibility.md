# Native API and test-double compatibility

Alloy composes Chromium, Brave and CEF before compiling. An added pure virtual
method or changed override must reach every mock, fake and concrete test fixture,
including fixtures only used by upstream test-support targets.

## Regular gate build

On the dedicated `wakaba-build.home` NUC, from `/home/builder/work/alloy` with
`TMPDIR=/home/builder/work/tmp`, run after the exact source tree's full apply:

```sh
python3 repo/tools/alloy_build.py --config release --gate --timeout 21600
python3 repo/tools/alloy_build.py --config verify --gate --timeout 21600
```

`--gate` builds the browser and existing guard, traffic, site-record and
navigation-mock tests. It also builds `alloy_api_test_support`: the real native
profile/device test-support dependencies, browser-window mocks, DNS test doubles,
Brave menu/layout fixtures and platform-app menu fixtures. Overrides local to
content's cache unit/browser-test files are compiled through their original
generated object targets. These are builds of the original translation units,
not copied test bodies or substituted production interfaces.

The separate native compatibility executable exercises an actual non-showing
context-menu fixture through its base interface, the configurable browser-window
query, a delayed cookie store's callback and native first-use behavior, and
actual layered-updater Mojo dispatch to both Viz display test clients:

```sh
xvfb-run -a src/out/Release_Alloy/alloy_api_compatibility_unittests \
  --ozone-platform=x11 --test-launcher-retry-limit=0
xvfb-run -a src/out/Verify_Alloy/alloy_api_compatibility_unittests \
  --ozone-platform=x11 --test-launcher-retry-limit=0 \
  --alloy-test-log-dir=/home/builder/work/alloy/evidence/api-test-guard-logs
```

Verification contexts use the ordinary browser-opened diagnostic log descriptor;
the product configuration compiles that path out. The gate's runtime checks,
XML/count assertions, protected smokes, inventory and product-artifact proof
remain separate required checks. Building support does not mean those upstream
test suites were executed.

## Audited API families

The audit covers Alloy's patch series and imported CEF composition changes, not
only the last failing fixture. Added default implementations are distinguished
from new concrete obligations. The stateless additions are audited alongside the
landed APIs; their forwarding defaults retain ordinary embedder/test behavior.

| API family | Test-double disposition |
|---|---|
| Context-menu `IsRunning()` | Chromium `TestRenderViewContextMenu`, Brave menu mock and platform-app menu fixture implement the state of their deliberately non-showing menus. Views-derived fixtures inherit the native runner query |
| Context-menu `IsMenuRunning()`, observer accelerator hooks and final `InitMenu` | Native view delegates implement the query; no separate test delegate lacks it. Observer defaults remain valid; test menu subclasses do not override the final assembly method |
| Browser-window `IsNormalBrowser()` | The shared mock exposes the new query with a type-derived default and explicit mock override. The Android-only fake is unaffected because the interface method is desktop-only |
| Browser-view layout dialog inset | Brave's no-top-UI fake keeps the supplied inset unchanged; the native delegate remains responsible for actual CEF adjustments |
| Cookie-store add/remove schemes | `DelayedCookieMonster` forwards to its real native child store, preserving callbacks and the first-use boundary. Derived CookieMonster fixtures inherit their implementation |
| Address-sorter socket tag | Both previously four-argument DNS mocks, their expectations and their callback actions accept the fifth argument. Other shared/delaying/endpoint fakes already implement it |
| DNS transaction/probe annotation inputs | Shared MockDnsTransactionFactory already implements the extended signatures and their defaults |
| Network-context configuration return value | Both cache test clients return the new success decision; the forwarding browser client propagates its base result |
| Network/SocketFactory Mojo annotation inputs and traffic options | Native TestNetworkContext, Ash network fake and mirroring socket fake already carry the composed signatures; their derived doubles inherit them |
| Navigation registration and priority | Mock handle and registry retain the previously integrated one-argument expectations and explicit priority path; native order/clear/reuse regressions remain in the gate |
| Render-widget external-parent methods | Native RenderWidgetHostViewBase implements both methods, inherited by the view test doubles |
| Viz output-surface proxy-device input | The composed TestOutputSurfaceProvider already implements the added parameter |
| Viz display-client platform guard and layered-updater damage rectangle | FakeDisplayClient and MockDisplayClient expose the now-cross-platform updater method; the Windows draw mock and its expectations preserve the damage rectangle and moved callback argument |
| Blink WebView picture-in-picture and WebSettings accessibility hooks | Concrete WebViewImpl/WebSettingsImpl provide the implementations; the audit found no separate abstract-interface test implementation requiring an update |
| New compositor delegate | The interface is newly introduced; there is no older test implementation missing its two methods |
| Linux printing delegate and Mojo receiver dispatch hooks | Printing methods retain their existing signatures through the extracted base; receiver-set's internal implementation already matches its composed callback type. No separate test implementation requires an update |
| Conditional machine-policy directory API | CEF omits DIR_POLICY_FILES and supplies EnablePlatformPolicySupport instead. Launcher/process fixtures use the native hook; the Linux token fixture retains its enrollment-file assertion with scoped restoration of the prior policy path. A native consumer case verifies both directory selection and token reading |
| Application/browser/view/theme/download/menu/modal/observer defaults | Added non-pure hooks keep native defaults; unchanged overrides remain signature-compatible |
| Removed P3A/referral and sponsored-component virtuals | TestingBraveBrowserProcess and FakeNTPBackgroundImagesService were already updated by the removal patches |
| Removed Origin namespace/features | The test-process teardown no longer shuts down a removed manager; the Android subscription fixture no longer enables a removed feature flag. Existing parser/security assertions remain intact |
| Stateless factory, renderer and owner hooks | The CEF factory has no separate test implementation; the renderer interface has no separate test receiver. Blink/platform, partition, permissions-client, Bluetooth/USB and external-file hooks have explicit ordinary-mode defaults, preserving existing doubles |

When changing an interface, inspect its indirect descendants and local fixtures
as well as files named `mock` or `fake`. Keep the production contract intact;
update the doubles and their expectations together. A compile-only failure in an
unused fixture is still an incomplete API change.
