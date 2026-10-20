#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Resolve the pinned Brave Origin build to Alloy, without Origin's product.

The applier calls discover()/render() in its baseline mirror. This command
publishes through that applier then audits its result; --audit is read-only.
Publication belongs exclusively to alloy_apply.py, including deletions and
local-edit protection.
Unknown conditional expressions fail closed rather than guessing a branch.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import socket
import subprocess
import sys

FLAG = 'is_brave_origin_branded'
CFLAG = 'IS_BRAVE_ORIGIN_BRANDED'
DELETE_DIRS = (
    'components/brave_origin', 'browser/brave_origin',
    'browser/ui/brave_origin', 'browser/ui/views/brave_origin',
    'browser/ui/webui/brave_origin_startup',
    'browser/resources/brave_origin_startup',
    'browser/resources/settings/brave_origin_page',
)
DELETE_FILES = ('components/brave_policy/brave_policy_profile_id.cc',)
EXTRA = (
    'build/BUILD.gn', 'chrome/BUILD.gn', 'chrome/app/BUILD.gn',
    'chrome/app/chrome_dll.rc',
)
BRAVE_EXTRA = (
    'app/brave_strings.grd',
    'components/brave_policy/brave_browser_policy_provider_unittest.cc',
    'browser/resources/settings/br/settings_menu.ts',
    'browser/resources/settings/brave_routes.ts',
    'chromium_src/chrome/browser/policy/profile_policy_connector_builder.cc',
    'chromium_src/chrome/browser/profiles/profile_impl.cc',
    'chromium_src/ios/chrome/browser/profile/model/profile_ios_impl.mm',
    'browser/resources/settings/brave_system_page/brave_system_page_index.html',
    'browser/resources/settings/br/basic_page.html',
    'browser/resources/settings/br/settings_main.html',
    'browser/resources/settings/br/route.ts',
    'browser/resources/settings/br/menu.ts',
    'build/args/branding_defaults.gni',
)
# These guards install Origin's purchase/startup UI, not its stripped features.
# Removing that product must not select the product's startup branch.
NO_PRODUCT_GUARDS = (
    'browser/brave_app_controller_mac.h', 'browser/brave_app_controller_mac.mm',
    'chromium_src/chrome/browser/ui/startup/startup_browser_creator.cc',
    'chromium_src/chrome/browser/ui/webui/chrome_web_ui_configs.cc',
    'browser/brave_content_browser_client.cc',
)
TEXT_SUFFIXES = {'.cc', '.h', '.mm', '.gn', '.gni', '.ts', '.tsx', '.js',
                 '.yaml', '.grd', '.grdp', '.rc', '.mojom', '.patch', '.html',
                 '.md', '.json5', '.spec'}


def deleted(name):
    return name in DELETE_FILES or any(name == d or name.startswith(d + '/')
                                       for d in DELETE_DIRS)


def discover(root):
    """Index-based discovery is stable after deletion and independent of mtimes."""
    brave = root / 'brave'
    p = subprocess.run(['git', 'grep', '--cached', '-Ilz', '-E',
                        'is_brave_origin_branded|BRAVE_ORIGIN|brave_origin|[Bb]raveOrigin|SetBraveProfilePolicyProviderProfileID'],
                       cwd=brave, capture_output=True, timeout=90)
    if p.returncode not in (0, 1):
        raise RuntimeError(p.stderr.decode())
    paths = {x.decode() for x in p.stdout.split(b'\0') if x}
    files = subprocess.check_output(['git', 'ls-files', '-z'], cwd=brave, timeout=60)
    for raw in files.split(b'\0'):
        if not raw:
            continue
        name = raw.decode()
        if deleted(name) or '/BRANDING' in name:
            paths.add(name)
    paths.update(n for n in BRAVE_EXTRA if (brave / n).is_file())
    staged_branding = {str(p.relative_to(root))
                       for branding in ('brave', 'brave_origin')
                       for p in (root / 'chrome/app/theme' / branding).glob('BRANDING*')}
    staged_branding.update(str(p.relative_to(root))
                           for p in (root / 'chrome/app').glob('*brave*strings.grd'))
    return sorted({'brave/' + n for n in paths
                   if deleted(n) or Path(n).suffix in TEXT_SUFFIXES
                   or Path(n).name.startswith('BRANDING')}
                  | set(EXTRA) | staged_branding)


def once(text, old, new=''):
    """Pinned, reviewable source surgery, idempotent on its exact output."""
    count = text.count(old)
    if count > 1:
        raise RuntimeError(f'Ambiguous rewrite: {old[:100]!r}')
    return text.replace(old, new) if count else text


def boolean(expr, c=False):
    """Partial boolean evaluation; unknown atoms retain their original spelling."""
    expr = expr.strip()
    # Peel only enclosing parentheses (not a function call's parentheses).
    while expr.startswith('(') and matching(expr, 0, '(', ')') == len(expr)-1:
        expr = expr[1:-1].strip()
    for operator in ('||', '&&'):
        depth = 0
        for i, char in enumerate(expr):
            depth += (char == '(') - (char == ')')
            if depth == 0 and expr[i:i+2] == operator:
                a, b = boolean(expr[:i], c), boolean(expr[i+2:], c)
                if operator == '&&':
                    if a is False or b is False:
                        return False
                    if a is True:
                        return b
                    if b is True:
                        return a
                else:
                    if a is True or b is True:
                        return True
                    if a is False:
                        return b
                    if b is False:
                        return a
                return f'({spell(a, c)}) {operator} ({spell(b, c)})'
    if expr.startswith('!'):
        value = boolean(expr[1:], c)
        return not value if isinstance(value, bool) else '!' + spell(value, c)
    if expr in ('true', '1', 'True'):
        return True
    if expr in ('false', '0', 'False'):
        return False
    return expr


def spell(value, c=False):
    if isinstance(value, bool):
        return ('1' if value else '0') if c else ('true' if value else 'false')
    return value


def matching(text, start, opening='{', closing='}'):
    """Balanced delimiters, ignoring strings and C/GN comments."""
    token = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/|\#[^\n]*|[{}()]', re.S)
    depth = 0
    for m in token.finditer(text, start):
        if m.group() == opening:
            depth += 1
        elif m.group() == closing:
            depth -= 1
            if depth == 0:
                return m.start()
    raise RuntimeError(f'Unbalanced {opening} at {text[start:start+100]!r}')


def cpp(text, product=True):
    """Resolve only flag-bearing preprocessor chains, preserving other guards."""
    lines = text.splitlines(keepends=True)

    def block(start, end):
        out = []
        i = start
        while i < end:
            m = re.match(r'\s*#\s*(if|ifdef|ifndef)\b(.*)', lines[i])
            if not m:
                out.append(lines[i])
                i += 1
                continue
            depth, j, boundaries = 1, i + 1, [i]
            while j < end:
                d = re.match(r'\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b', lines[j])
                if d:
                    word = d[1]
                    if word in ('if', 'ifdef', 'ifndef'):
                        depth += 1
                    elif word == 'endif':
                        depth -= 1
                        if not depth:
                            break
                    elif depth == 1:
                        boundaries.append(j)
                j += 1
            if depth:
                raise RuntimeError('Unbalanced C preprocessor chain')
            boundaries.append(j)
            flag_chain = any(CFLAG in lines[k] for k in boundaries[:-1])
            if not flag_chain:
                for a, b in zip(boundaries, boundaries[1:]):
                    out.append(lines[a])
                    out.append(block(a+1, b))
                out.append(lines[j])
            else:
                remaining = False
                for a, b in zip(boundaries, boundaries[1:]):
                    directive = re.match(r'\s*#\s*(if|ifdef|ifndef|elif|else)\b(.*)', lines[a])
                    kind, expression = directive[1], directive[2].split('//')[0].strip()
                    if kind == 'else':
                        value = True
                    else:
                        expression = re.sub(r'BUILDFLAG\(IS_BRAVE_ORIGIN_BRANDED\)|defined\(IS_BRAVE_ORIGIN_BRANDED\)',
                                            '1' if product else '0', expression)
                        if kind in ('ifdef', 'ifndef') and expression == CFLAG:
                            expression = '1' if product else '0'
                            if kind == 'ifndef':
                                expression = '!' + expression
                        value = boolean(expression, c=True)
                    if value is False:
                        continue
                    if value is True:
                        if remaining:
                            out.append('#else\n')
                        out.append(block(a+1, b))
                        break
                    out.append(('#elif ' if remaining else '#if ') + spell(value, True) + '\n')
                    out.append(block(a+1, b))
                    remaining = True
                if remaining:
                    out.append('#endif\n')
            i = j + 1
        return ''.join(out)
    result = block(0, len(lines))
    return result.replace('BUILDFLAG(IS_BRAVE_ORIGIN_BRANDED)', 'true')


def gn(text):
    # Imports/defines of a deleted branding flag have no remaining consumer.
    text = re.sub(r'^.*(?:import\("//brave/components/brave_origin/|"IS_BRAVE_ORIGIN_BRANDED").*\n', '', text, flags=re.M)
    text = re.sub(r'^.*\bis_brave_origin_branded\s*=.*\n', '', text, flags=re.M)
    text = text.replace('$is_brave_origin_branded', 'true')
    text = re.sub(r'\bis_brave_origin_branded\b', 'true', text)
    text = gn_chains(text)
    # Constant-fold only lines affected by Origin (e.g. platform && !true).
    lines = []
    for line in text.splitlines(keepends=True):
        if re.search(r'\btrue\b', line) and '=' in line and not re.search(r'["\']', line):
            m = re.match(r'(\s*\w+\s*=\s*)([^\n#]+)(.*)', line)
            if m:
                line = m[1] + spell(boolean(m[2])) + m[3] + ('\n' if line.endswith('\n') else '')
        lines.append(line)
    return re.sub(r'(?m)^[ \t]+$', '', ''.join(lines))


def gn_chains(text):
    """Resolve complete chains as one node, with nested chains as children."""
    mask = re.sub(r'"(?:\\.|[^"\\])*"|\#[^\n]*',
                  lambda m: re.sub(r'[^\n]', ' ', m.group()), text)
    out, position = [], 0
    while found := re.search(r'\bif\s*\(', mask[position:]):
        start = position + found.start()
        out.append(text[position:start])
        cursor, arms = start, []
        while True:
            paren = mask.index('(', cursor)
            close = matching(mask, paren, '(', ')')
            expression = text[paren+1:close]
            brace = close + 1
            while brace < len(mask) and mask[brace].isspace():
                brace += 1
            if mask[brace:brace+1] != '{':
                raise RuntimeError('Expected GN conditional block')
            end = matching(mask, brace)
            condition = boolean(expression) if re.search(r'\b(?:true|false)\b', expression) else expression.strip()
            arms.append((condition, text[brace+1:end]))
            after = end + 1
            tail = re.match(r'\s*else\b\s*', mask[after:])
            if not tail:
                break
            other = after + tail.end()
            if mask[other:other+2] == 'if':
                cursor = other
                continue
            if mask[other:other+1] != '{':
                raise RuntimeError('Unknown GN else syntax')
            end = matching(mask, other)
            arms.append((True, text[other+1:end]))
            after = end + 1
            break
        remaining = []
        for condition, body in arms:
            if condition is False:
                continue
            body = gn_chains(body)
            if condition is True:
                remaining.append(('else {', body) if remaining else ('', body))
                break
            remaining.append((('else if' if remaining else 'if') + f' ({condition}) {{', body))
        out.append(' '.join((header + body + '}') if header else body
                            for header, body in remaining))
        position = after
    out.append(text[position:])
    return ''.join(out)


def xml_conditions(text):
    """Resolve GRIT's XML and comment directives without reserializing XML/TS."""
    pattern = re.compile(r'(?P<open><if\s+expr="(?P<expr>[^"]+)"\s*>)|(?P<close></if>)')
    matches = list(pattern.finditer(text))
    stack, changes = [], []
    for m in matches:
        if m['open']:
            stack.append(m)
        else:
            if not stack:
                raise RuntimeError('Unbalanced GRIT condition')
            start = stack.pop()
            if FLAG not in start['expr']:
                continue
            expression = start['expr'].replace(FLAG, 'true')
            expression = re.sub(r'\bnot\b', '!', expression)
            expression = re.sub(r'\band\b', '&&', expression)
            expression = re.sub(r'\bor\b', '||', expression)
            value = boolean(expression)
            # Include a TS // prefix and whole-line whitespace for removed tags.
            a = text.rfind('\n', 0, start.start()) + 1
            b = text.rfind('\n', 0, m.start()) + 1
            is_line = text[a:start.start()].strip() in ('', '//')
            if value is False:
                changes.append((a if is_line else start.start(),
                                text.find('\n', m.end())+1 if is_line else m.end(), ''))
            elif value is True:
                changes.append((a if is_line else start.start(),
                                text.find('\n', start.end())+1 if is_line else start.end(), ''))
                changes.append((b if is_line else m.start(),
                                text.find('\n', m.end())+1 if is_line else m.end(), ''))
            else:
                expression = spell(value).replace('&&', 'and').replace('||', 'or')
                expression = re.sub(r'!\s*', 'not ', expression)
                changes.append((start.start(), start.end(), f'<if expr="{expression}">'))
    if stack:
        raise RuntimeError('Unbalanced GRIT condition')
    # Outermost removals dominate nested changes.
    selected = []
    for change in sorted(changes, key=lambda c: (c[0], -c[1])):
        if not selected or change[0] >= selected[-1][1]:
            selected.append(change)
    for a, b, body in reversed(selected):
        text = text[:a] + body + text[b:]
    return text


def ts_conditions(text):
    """Brave's ifdef-loader uses line-based if/elif/else/end directives.

    Parse the whole chain with the preprocessor resolver, retaining directives
    for unrelated build conditions. Treating these as XML pairs loses <else>.
    """
    def opening(m):
        expression = m[3].replace(FLAG, 'BUILDFLAG(IS_BRAVE_ORIGIN_BRANDED)')
        return m[1] + '#' + m[2] + ' ' + expression + '\n'
    text = re.sub(r'^([ \t]*)// <(if|elif) expr="([^"]+)">[ \t]*\n', opening, text, flags=re.M)
    text = re.sub(r'^([ \t]*)// <else>[ \t]*\n', r'\1#else\n', text, flags=re.M)
    text = re.sub(r'^([ \t]*)// </if>[ \t]*\n', r'\1#endif\n', text, flags=re.M)
    text = cpp(text)
    text = re.sub(r'^([ \t]*)#(if|elif) ([^\n]*)\n', r'\1// <\2 expr="\3">\n', text, flags=re.M)
    text = re.sub(r'^([ \t]*)#else\n', r'\1// <else>\n', text, flags=re.M)
    return re.sub(r'^([ \t]*)#endif[^\n]*\n', r'\1// </if>\n', text, flags=re.M)


def patch_text(text, name):
    """Transform each new-side hunk then regenerate counts/context as a diff.

    Never treat +/- prefixes as source syntax or leave stale hunk lengths.
    Old-side bytes are preserved, including upstream context lines.
    """
    lines = text.splitlines(keepends=True)
    out, i, delta = [], 0, 0
    while i < len(lines):
        m = re.match(r'@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)', lines[i])
        if not m:
            out.append(lines[i])
            i += 1
            continue
        old_start, new_start = int(m[1]), int(m[3]) + delta
        i += 1
        hunk = []
        while i < len(lines) and lines[i][:1] in (' ', '+', '-', '\\') and not lines[i].startswith('--- '):
            hunk.append(lines[i])
            i += 1
        old = ''.join(s[1:] for s in hunk if s.startswith((' ', '-')))
        new = ''.join(s[1:] for s in hunk if s.startswith((' ', '+')))
        if 'brave_origin' not in new and CFLAG not in new:
            out.append(f'@@ -{m[1]},{m[2] or 1} +{new_start},{m[4] or 1} @@{m[5]}\n')
            out.extend(hunk)
            continue
        # Added-side fragments may have an unrelated open #if at the edge of
        # their context. Resolve complete added runs, retaining context exactly.
        rewritten = []
        k = 0
        while k < len(hunk):
            if not hunk[k].startswith('+'):
                rewritten.append(hunk[k])
                k += 1
                continue
            j = k
            while j < len(hunk) and hunk[j].startswith('+'):
                j += 1
            source = ''.join(s[1:] for s in hunk[k:j])
            if CFLAG in source or 'brave_origin/buildflags' in source:
                if name.endswith('.rc.patch'):
                    # The added outer #if 0 intentionally wraps existing RC.
                    tail = '#if 0\n' if source.endswith('#if 0\n') else ''
                    source = cpp(source[:-len(tail)] if tail else source) + tail
                else:
                    source = gn(source)
            source = remove_dependencies(source)
            rewritten.extend('+' + line for line in source.splitlines(keepends=True))
            k = j
        new = ''.join(s[1:] for s in rewritten if s.startswith((' ', '+')))
        before_count = int(m[4] or 1)
        after_count = len(new.splitlines())
        # Git rejects context-only hunks. Removing a complete added branch
        # removes its hunk, while its line delta still shifts later hunks.
        if any(line.startswith(('+', '-')) for line in rewritten):
            out.append(f'@@ -{old_start},{len(old.splitlines())} +{new_start},{after_count} @@{m[5]}\n')
            out.extend(rewritten)
        delta += after_count - before_count
    return ''.join(out)


def remove_dependencies(text):
    # Remove whole GN list elements, imports and C includes, never partial tokens.
    text = re.sub(r'^\s*#include ["<]brave/(?:components/brave_origin|browser/brave_origin|browser/ui/(?:views/)?brave_origin)[^\n]*\n', '', text, flags=re.M)
    text = re.sub(r'^.*import\("//brave/components/brave_origin/[^\n]*\n', '', text, flags=re.M)
    text = re.sub(r'"(?:[^"\n]*/)?brave/(?:components/brave_origin|browser/brave_origin|browser/ui/(?:views/)?brave_origin|browser/ui/webui/brave_origin_startup|browser/resources/brave_origin_startup)(?:[/:][^"\n]*)?"[ \t]*,?[ \t]*', '', text)
    return text


def render(name, body):
    """Return desired bytes/None for one managed Chromium-relative path."""
    local = name.removeprefix('brave/')
    if name.startswith('brave/') and deleted(local):
        return None
    if body is None:
        return None
    try:
        text = body.decode('utf-8')
    except UnicodeDecodeError:
        return body
    # Product-specific rewrites are kept separate from syntax resolution.
    text = product_code(local, text)
    suffix = Path(name).suffix
    if suffix == '.patch':
        text = patch_text(text, name)
    else:
        if suffix in ('.cc', '.h', '.mm', '.rc', '.mojom') and CFLAG in text:
            text = cpp(text, local not in NO_PRODUCT_GUARDS)
        if suffix in ('.gn', '.gni'):
            text = gn(text)
            text = remove_dependencies(text)
            text = re.sub(r'(?m)[ \t]+$', '', text)
        elif suffix in ('.cc', '.h', '.mm', '.mojom'):
            text = remove_dependencies(text)
        if suffix in ('.ts', '.tsx') and FLAG in text:
            text = ts_conditions(text)
        elif suffix in ('.grd', '.grdp', '.html') and FLAG in text:
            text = xml_conditions(text)
    return text.encode()


def cut_between(text, first, last, replacement=''):
    if first not in text:
        return text
    if text.count(first) != 1 or text.count(last) != 1:
        raise RuntimeError(f'Ambiguous source section: {first!r} / {last!r}')
    start = text.index(first)
    finish = text.index(last, start)
    return text[:start] + replacement + text[finish:]


def cut_function(text, pattern, replacement=''):
    matches = list(re.finditer(pattern, text, re.M))
    for m in reversed(matches):
        brace = text.index('{', m.start())
        finish = matching(text, brace) + 1
        text = text[:m.start()] + replacement + text[finish:]
    return text


def product_code(name, text):
    """Remove the purchase product's owners and the callers of those owners.

    Keep the unrelated AdBlockOnlyMode policy provider and its observation;
    it must no longer wait for an Origin profile ID or Origin readiness.
    """
    if Path(name).name.startswith('BRANDING'):
        text = re.sub(r'^(PRODUCT_(?:FULLNAME|SHORTNAME))=.*$', r'\1=Alloy', text, flags=re.M)
        text = re.sub(r'^(PRODUCT_INSTALLER_(?:FULLNAME|SHORTNAME))=.*$', r'\1=Alloy Installer', text, flags=re.M)
    if name.endswith(('brave_strings.grd', 'brave_origin_strings.grd')):
        text = re.sub(r'(<message\b[^>]*name="IDS_(?:SHORT_PRODUCT_NAME|PRODUCT_NAME|APP_MENU_PRODUCT_NAME)"[^>]*>)\s*[^<]*?(\s*</message>)',
                      r'\1Alloy\2', text)
        text = text.replace('Brave Origin', 'Alloy')
    if name == 'build/config.gni':
        text = text.replace('_product_brand = "Origin"', '_product_brand = "Alloy"')
        text = text.replace('brave_dist_name = "brave-origin"', 'brave_dist_name = "alloy"')
        text = text.replace('brave_linux_package_name = "brave-origin"', 'brave_linux_package_name = "alloy"')
    if name == 'browser/brave_browser_process_impl.cc':
        text = cut_between(text, '  // Initialize BraveOriginPolicyManager here', '\n}\n\nvoid BraveBrowserProcessImpl::PreMainMessageLoopRun')
        text = re.sub(r'^  // Reset BraveOriginPolicyManager[^\n]*\n  brave_origin::[^\n]*\n', '', text, flags=re.M)
    if name == 'browser/browser_context_keyed_service_factories.cc':
        text = re.sub(r'^  brave_origin::BraveOriginServiceFactory::GetInstance\(\);\n', '', text, flags=re.M)
    if name in ('browser/brave_local_state_prefs.cc', 'browser/brave_profile_prefs.cc'):
        text = re.sub(r'^  brave_origin::(?:prefs::)?Register\w+Prefs\(registry\);\n', '', text, flags=re.M)
    if name == 'browser/about_flags.cc':
        text = re.sub(r'  EXPAND_FEATURE_ENTRIES\(\{\s*\\\n\s*"brave-origin",.*?\}\)\s*\\\n', '', text, flags=re.S)
    if name == 'chromium_src/chrome/browser/profiles/profile.cc':
        text = cut_between(text, '// This is to avoid a circular dep', 'namespace brave_origin {')
        text = re.sub(r'namespace brave_origin \{.*?\}  // namespace brave_origin\n', '', text, flags=re.S)
    if name == 'chromium_src/chrome/browser/policy/profile_policy_connector_builder.cc':
        # The wrapper only existed to derive and forward Origin's stable ID.
        text = text[:text.index('#include')] + '#include <chrome/browser/policy/profile_policy_connector_builder.cc>  // IWYU pragma: export\n'
    if name == 'chromium_src/chrome/browser/profiles/profile_impl.cc':
        text = re.sub(r'#if BUILDFLAG\(IS_ANDROID\)\nnamespace brave_policy \{.*?#endif[^\n]*\n', '', text, flags=re.S)
        text = re.sub(r'#if BUILDFLAG\(IS_ANDROID\)\n#undef BRAVE_PROFILE_IMPL_TAKE_PREFS_FROM_STARTUP_DATA\n#endif[^\n]*\n', '', text)
    if name == 'chromium_src/ios/chrome/browser/profile/model/profile_ios_impl.mm':
        text = text[:text.index('#include')] + '#include <ios/chrome/browser/profile/model/profile_ios_impl.mm>\n'
    if name == 'browser/ai_chat/ai_chat_agent_profile_manager.cc':
        text = cut_between(text, '  // Brave Origin manages AI Chat', '  // The remaining setup only applies')
    if name == 'browser/ai_chat/ai_chat_agent_profile_browsertest.cc':
        # These three cases exercise only the removed purchase-policy product.
        for m in reversed(list(re.finditer(r'^IN_PROC_BROWSER_TEST_F\([^{}]*\)\s*\{', text, re.M))):
            end = matching(text, text.index('{', m.start())) + 1
            if 'brave_origin::' in text[m.start():end]:
                text = text[:m.start()] + text[end:]
    if name == 'browser/brave_stats/brave_stats_updater.cc':
        text = cut_between(text, '  // Observe `BraveOriginPolicyManager`', '  const base::CommandLine& command_line')
        text = text.replace('brave_policies_ready_ = false', 'brave_policies_ready_ = true')
    if name == 'browser/brave_stats/brave_stats_updater.h':
        text = re.sub(r'  base::ScopedObservation<brave_origin::BraveOriginPolicyManager,\n\s*brave_policy::BravePolicyObserver>\n\s*policy_manager_observation_\{this\};\n', '', text)
        text = text.replace('bool brave_policies_ready_ = false;', 'bool brave_policies_ready_ = true;')
    if name.startswith('components/brave_policy/brave_'):
        text = re.sub(r'  // Register as BraveOriginPolicyManager observer\..*?\n\s*(?:policy_manager|brave_origin)_observation_\.Observe\(\n\s*brave_origin::BraveOriginPolicyManager::GetInstance\(\)\);\n', '', text, flags=re.S)
        for function in ('LoadBraveOriginPolicies', 'LoadBraveOriginPolicy', 'OnProfilePolicyChanged', 'SetProfileID'):
            text = cut_function(text, r'^void Brave(?:Browser|Profile)PolicyProvider::' + function + r'\(')
        text = re.sub(r'  if \(brave_origin::IsBraveOriginPurchased\(\)\) \{\n    LoadBraveOriginPolicies\(bundle\);\n  \}\n', '', text)
        text = re.sub(r'  // Helper to (?:set|load) BraveOrigin[^\n]*\n.*?\n\n', '', text, flags=re.S)
        text = re.sub(r'  base::ScopedObservation<brave_origin::BraveOriginPolicyManager,\n\s*BravePolicyObserver>\n\s*\w+_observation_\{this\};\n', '', text)
        if name.endswith('brave_browser_policy_provider.cc'):
            for function in ('OnBravePoliciesReady', 'OnBrowserPolicyChanged'):
                text = cut_function(text, r'^void BraveBrowserPolicyProvider::' + function + r'\(')
            text = cut_function(text, r'^void BraveBrowserPolicyProvider::Init\(',
                                'void BraveBrowserPolicyProvider::Init(policy::SchemaRegistry* registry) {\n  policy::ConfigurationPolicyProvider::Init(registry);\n  RefreshPolicies(policy::PolicyFetchReason::kBrowserStart);\n}')
        if name.endswith('brave_browser_policy_provider.h'):
            text = once(text, ' : public policy::ConfigurationPolicyProvider,\n                                   public BravePolicyObserver', ' : public policy::ConfigurationPolicyProvider')
            text = re.sub(r'  // BravePolicyObserver implementation\.\n  void OnBravePoliciesReady\(\) override;\n  void OnBrowserPolicyChanged\(std::string_view policy_key\) override;\n', '', text)
        if name.endswith('brave_profile_policy_provider.cc'):
            text = cut_function(text, r'^bool BraveProfilePolicyProvider::IsInitializationComplete\(',
                                'bool BraveProfilePolicyProvider::IsInitializationComplete(\n    policy::PolicyDomain domain) const {\n  return first_policies_loaded_;\n}')
            text = cut_function(text, r'^void BraveProfilePolicyProvider::OnBravePoliciesReady\(',
                                'void BraveProfilePolicyProvider::OnBravePoliciesReady() {\n  RefreshPolicies(first_policies_loaded_\n                      ? policy::PolicyFetchReason::kUserRequest\n                      : policy::PolicyFetchReason::kBrowserStart);\n}')
            # Publish even in upstream processes that do not initialise Brave's
            # optional policy manager; later notifications refresh that bundle.
            old = '      AdBlockOnlyModePolicyManager::GetInstance());'
            if old + '\n  RefreshPolicies(' not in text:
                text = once(text, old, old + '\n  RefreshPolicies(policy::PolicyFetchReason::kBrowserStart);')
        if name.endswith('brave_profile_policy_provider.h'):
            text = re.sub(r'  void OnProfilePolicyChanged\(std::string_view policy_key,\n\s*std::string_view profile_id\) override;\n  void SetProfileID\(const std::string& profile_id\);\n', '', text)
            text = re.sub(r'  bool policies_ready_ = false;\n  std::string profile_id_;\n', '', text)
    if name in ('components/brave_policy/brave_profile_policy_provider_unittest.cc',
                'components/brave_policy/brave_browser_policy_provider_unittest.cc'):
        profile = 'brave_profile_' in name
        text = re.sub(r'^  provider_\.SetProfileID\([^\n]*\n', '', text, flags=re.M)
        # The provider now publishes its initial (possibly empty) policy bundle
        # synchronously; there is no purchase/ID readiness barrier.
        text = once(text, '  // Policies should still not be loaded until observer event fires\n  EXPECT_FALSE(',
                    '  // The initial bundle is published synchronously.\n  EXPECT_TRUE(')
        if profile:
            text = cut_function(text, r'^  BraveProfilePolicyProviderTest\(\)\s*\{',
                                '  BraveProfilePolicyProviderTest() = default;')
            text = cut_function(text, r'^  void TearDown\(\) override',
                                '  void TearDown() override { provider_.Shutdown(); }')
            text = cut_function(text, r'^TEST_F\(BraveProfilePolicyProviderTest, IsInitializationCompleteGated\)',
                                'TEST_F(BraveProfilePolicyProviderTest, InitializationNeedsNoProductIdentity) {\n  EXPECT_FALSE(provider_.IsInitializationComplete(policy::POLICY_DOMAIN_CHROME));\n  provider_.Init(&schema_registry_);\n  EXPECT_TRUE(provider_.IsInitializationComplete(policy::POLICY_DOMAIN_CHROME));\n  EXPECT_TRUE(provider_.IsFirstPolicyLoadComplete(policy::POLICY_DOMAIN_CHROME));\n}')
            text = cut_function(text, r'^TEST_F\(BraveProfilePolicyProviderTest,\s*OnProfilePolicyChanged_\w+\)')
        else:
            text = re.sub(r'^  provider_\.OnBravePoliciesReady\(\);\n', '', text, flags=re.M)
            text = cut_function(text, r'^TEST_F\(BraveBrowserPolicyProviderTest, OnBrowserPolicyChanged\)')
        text = text.replace('// Fire the observer event and set profile ID to trigger policy loading',
                            '// Refresh through the retained AdBlockOnlyMode notification')
    if name == 'components/brave_policy/BUILD.gn':
        text = once(text, '    "brave_policy_profile_id.cc",\n')
    if name == 'browser/brave_content_browser_client.cc':
        text = cut_function(text, r'^void BindOriginIAPSubscription\(')
        text = once(text, '  registry.ForWebUI<BraveSettingsUI>()\n      .Add<brave_origin::mojom::BraveOriginSettingsHandler>();\n')
        text = cut_function(text, r'^  if \(base::FeatureList::IsEnabled\(brave_origin::features::kBraveOrigin\)\)')
    if name == 'renderer/brave_content_renderer_client.cc':
        text = once(text, '  should_create_subscription_observer |=\n      base::FeatureList::IsEnabled(brave_origin::features::kBraveOrigin);\n')
    if name == 'browser/ui/webui/brave_settings_ui.cc':
        text = cut_between(text, '  html_source->AddBoolean("isBraveOriginBrandedBuild",', '  html_source->AddBoolean("isTreeTabsFlagEnabled",')
        text = cut_function(text, r'^void BraveSettingsUI::BindInterface\(\n    mojo::PendingReceiver<brave_origin::mojom::BraveOriginSettingsHandler>')
        text = cut_between(text, '  auto* brave_origin_service =', '  html_source->AddBoolean("isPsstEnabled",')
        text = text.replace(' &&\n                                                is_managed_by_brave_origin', '')
    if name == 'browser/ui/webui/brave_settings_ui.h':
        text = once(text, '  void BindInterface(\n      mojo::PendingReceiver<brave_origin::mojom::BraveOriginSettingsHandler>\n          pending_receiver);\n')
    if name == 'browser/ui/webui/settings/brave_privacy_handler.cc':
        text = cut_between(text, '  auto* brave_origin_service =', '#endif\n\n  data_source->AddBoolean(')
        text = text.replace(' &&\n          !is_managed_by_brave_origin', '')
    if name == 'browser/resources/settings/br/settings_ui.ts':
        text = cut_between(text, "import * as BraveOriginMojom", '// TODO: move throttle utility')
    if name in ('browser/resources/settings/br/basic_page.ts', 'browser/resources/settings/br/settings_main.ts'):
        text = re.sub(r'^import .*brave_origin_page[^\n]*\n', '', text, flags=re.M)
    if name == 'browser/resources/settings/br/settings_main.ts':
        text = cut_between(text, '    // Insert the origin page into the view manager', '    // Insert the content page into the view manager')
    if name == 'browser/resources/settings/br/page_visibility.ts':
        text = once(text, "    origin: loadTimeData.getBoolean('isBraveOriginPurchased') &&\n            !loadTimeData.getBoolean('isBraveOriginBrandedBuild'),\n")
    if name == 'browser/resources/settings/br/settings_menu.ts':
        text = cut_between(text, '    // Brave Origin\n', '    // Move Appearance item')
        text = once(text, '      // Insert after Origin if visible, otherwise after Get Started\n      const insertAfter = originEl.hidden ? getStartedEl : originEl\n      insertAfter.insertAdjacentElement',
                    '      getStartedEl.insertAdjacentElement')
    if name == 'browser/resources/settings/brave_routes.ts':
        text = re.sub(r'^.*r\.ORIGIN = [^\n]*\n', '', text, flags=re.M)
    if name == 'browser/ui/webui/settings/brave_settings_localized_strings_provider.cc':
        text = cut_between(text, '      {"braveOriginTitle",', '      {"siteSettingsShields",')
    if name == 'browser/resources/settings/brave_system_page/brave_system_page_index.ts':
        text = re.sub(r'^import .*brave_origin[^\n]*\n', '', text, flags=re.M)
        text = re.sub(r'      showOriginOnboarding_: \{.*?\n      \},\n', '', text, flags=re.S)
        text = once(text, '  declare private showOriginOnboarding_: boolean;\n')
        text = cut_between(text, '  private braveOriginHandler_:', '  constructor()')
        text = cut_between(text, '    // If Origin onboarding is enabled,', '\n  }\n\n  override disconnectedCallback()')
        text = cut_between(text, '  override disconnectedCallback()', '  private hasBatteryChanged_')
        text = cut_function(text, r'^    if \(this.showOriginOnboarding_\)')
    if name == 'browser/resources/settings/brave_system_page/brave_system_page_index.html':
        text = re.sub(r'  <template is="dom-if" if="\[\[showOriginOnboarding_\]\]">.*?</template>\n', '', text, flags=re.S)
    if name == 'browser/resources/settings/sources.gni':
        text = re.sub(r'^.*(?:"brave_origin_page/|brave_origin_settings\.mojom|brave_origin/mojom)[^\n]*\n', '', text, flags=re.M)
    if name == 'rewrite/build/BUILD.gn.yaml':
        text = text[:text.index('substitutions:')] + 'substitutions: []\n'
    if name == 'rewrite/chrome/app/BUILD.gn.yaml':
        text = text.split('  # Define IS_BRAVE_ORIGIN_BRANDED', 1)[0]
    if name == 'rewrite/chrome/BUILD.gn.yaml':
        text = re.sub(r'  - description: Make the Brave Origin buildflags available\..*?\n\n', '', text, flags=re.S)
        text = once(text, '      Also defines `IS_BRAVE_ORIGIN_BRANDED` for Brave Origin builds.\n')
        text = once(text, '          if (is_brave_origin_branded) {\n            defines += [ "IS_BRAVE_ORIGIN_BRANDED" ]\n          }\n')
    if name == 'build/commands/lib/buildArgs.ts':
        text = once(text, "  'is_brave_origin_branded',\n")
    if name == 'build/commands/lib/config.ts':
        text = re.sub(r"this.isBraveOriginBranded = envConfig.getBoolean\(\[\s*'is_brave_origin_branded',\s*[^\]]*\]\)",
                      'this.isBraveOriginBranded = true', text)
    if name == 'components/brave_search_conversion/utils.cc':
        text = text.replace('ShouldSuppressForBraveOrigin', 'ShouldSuppressSearchPromotion')
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--audit', action='store_true')
    args = parser.parse_args()
    root = args.root.resolve()
    if socket.gethostname() != 'wakaba-build' or root != Path('/home/builder/work/alloy/src'):
        parser.error('Only the approved NUC Alloy source tree is supported')
    if args.report and args.report.resolve().is_relative_to(root):
        parser.error('Report must be outside src')
    if not args.audit:
        subprocess.run([sys.executable, str(Path(__file__).with_name('alloy_apply.py')),
                        '--root', str(root)], check=True, timeout=300)
    changes, tree = [], hashlib.sha256()
    for name in discover(root):
        p = root / name
        before = p.read_bytes() if p.exists() else None
        after = render(name, before)
        if before != after:
            changes.append(name)
            print('WOULD_DELETE' if after is None else 'WOULD_CHANGE', name)
        tree.update(name.encode() + b'\0' + (hashlib.sha256(before).digest() if before is not None else b'absent'))
    report = dict(changes=changes, tree_sha256=tree.hexdigest(), mode='audit')
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, sort_keys=True))
    return bool(changes)


if __name__ == '__main__':
    raise SystemExit(main())
