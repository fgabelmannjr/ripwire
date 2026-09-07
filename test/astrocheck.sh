#!/usr/bin/env bash
# astrocheck.sh — the gate for Astro (.astro) coverage on the TypeScript grammar.
#
#   test/astrocheck.sh
#   RIPWIRE_BIN=asan/ripwire test/astrocheck.sh
#
# THE FAILURE THIS PINS: before this round `.astro` was `unsupported-ext` — no grammar, no doc handler —
# so every component, layout and page of an Astro site was absent from the map, and `--callers` of a
# layout returned nothing. Measured 2026-09-07 on 8 real .astro files fed to the TypeScript grammar
# (docs/plans/astro-support.md): the RAW file parses degraded 8/8 and mints junk (`const` as a symbol,
# 2 of 8 files lose every import row); the same file with everything outside the frontmatter and
# <script> bodies blanked to spaces parses clean 8/8 and keeps every import. So `.astro` rides
# Lang::TypeScript + tree_sitter_typescript + the typescript tags.scm behind a region blanker
# (src/ingest_astro.h), plus a post-pass that emits what no TS query can see: the component itself
# (the file stem, SymKind::Class, whole-file span) and one call reference per PascalCase template tag.
#
# The fixture test/astrofix/ is the smallest tree carrying every construct the decision rests on:
#   layouts/Layout.astro      — frontmatter interface + imports + a helper call; <Header/> and <slot/>
#                               and <Fragment> in the template; a <style> block; a <script> block with a
#                               named client function (focusMain).
#   components/Header.astro   — imports a React island by EXTENSION-LESS specifier ('./Nav') and renders
#                               <Nav client:load /> — the tag→import→.tsx chain.
#   components/Nav.tsx        — the island; calls lib/util.ts::helper (an ordinary TS edge, unchanged).
#   pages/index.astro         — imports Layout WITH the .astro extension; a JSON-LD `is:inline` script
#                               that must contribute nothing; a <Fragment> that must never be a symbol.
#   pages/es/index.astro      — a SECOND `index` stem (i18n routing): two rows, distinct ids.
#   pages/404.astro           — a digit-led stem; still a page symbol.
#   pages/blog/[...slug].astro — a dynamic-route stem: NOT an identifier, so no page symbol, but its
#                               frontmatter (getStaticPaths) and its import still index.
#
# Exit 0 = ALL PASS; the trailing python3 heredocs make the interpreter rc the gate rc (set -e).

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-${RIPWIRE_BIN:-$ROOT/build/ripwire}}"
[ "${BIN#/}" = "$BIN" ] && BIN="$ROOT/$BIN"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir "$TMP/cache"
export XDG_CACHE_HOME="$TMP/cache"
[ -x "$BIN" ] || { echo "no ripwire binary at $BIN — build first (cmake --build build -j)"; exit 2; }
cp -R "$ROOT/test/astrofix" "$TMP/fix"

echo "astrocheck: BIN=$BIN"

# ── 1) the premise: .astro is crawled, indexed, and no longer reported as unsupported ─────────────────
"$BIN" "$TMP/fix" --no-cache > "$TMP/map.xml"
"$BIN" "$TMP/fix" --no-cache --skipped > "$TMP/skipped.xml"
python3 - "$TMP/map.xml" "$TMP/skipped.xml" <<'PY'
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
files = {f.get('p') for f in root.iter('f')}
for want in ('layouts/Layout.astro', 'components/Header.astro', 'pages/index.astro',
             'pages/es/index.astro', 'pages/404.astro', 'pages/blog/[...slug].astro'):
    assert want in files, ('astro file absent from the map', want, sorted(files))
skipped = ET.parse(sys.argv[2]).getroot()
astro_drops = [f.get('p') for f in skipped.iter('f') if f.get('ext') == '.astro']
assert not astro_drops, ('--skipped still drops .astro', astro_drops)
print('  PASS .astro files are crawled and indexed; --skipped no longer lists them')
PY

# ── 2) symbols: components, pages, frontmatter, <script> functions — and NO junk ──────────────────────
python3 - "$TMP/map.xml" <<'PY'
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
rows = list(root.iter('s'))
byname = {}
for s in rows:
    byname.setdefault(s.get('n'), []).append(s)
def kinds(name): return sorted(s.get('t') for s in byname.get(name, []))
# component / page symbols: the file stem, class-kind, one row per file
assert kinds('Layout') == ['cls'], ('Layout', kinds('Layout'))
assert kinds('Header') == ['cls'], ('Header', kinds('Header'))
assert kinds('index') == ['cls', 'cls'], ('two index pages expected (i18n)', kinds('index'))
assert kinds('404') == ['cls'], ('404 page', kinds('404'))
owners = {f.get('p') for f in root.iter('f') for s in f.iter('s') if s.get('n') == 'index'}
assert owners == {'pages/index.astro', 'pages/es/index.astro'}, ('the two index pages must be two rows under their own files', owners)
# a dynamic-route stem is not an identifier: no page symbol, frontmatter still indexes
assert '[...slug]' not in byname, 'bracket stem minted a symbol'
assert 'getStaticPaths' in byname and kinds('getStaticPaths') == ['fn'], kinds('getStaticPaths')
# frontmatter + script bodies parse as ordinary TypeScript
assert kinds('Props') == ['iface', 'iface'], ('Props in Layout and Header', kinds('Props'))
assert kinds('focusMain') == ['fn'], ('client <script> function', kinds('focusMain'))
assert kinds('formatTitle') == ['fn'] and kinds('helper') == ['fn'] and kinds('Nav') == ['fn'], 'plain TS rows moved'
# nothing from the template, the <style> block, or the JSON-LD script becomes a symbol
for junk in ('slot', 'Fragment', 'script', 'style', 'html', 'head', 'body', 'main', 'header', 'div',
             'const', 'jsonLdKey', 'hero', 'title', 'description', 'pageTitle'):
    assert junk not in byname, ('junk symbol minted from outside the TS regions', junk)
print('  PASS component/page symbols by stem, frontmatter and <script> TS, dynamic-route stem refused, no junk')
PY

# ── 3) THE ACCEPTANCE CASE: PascalCase template tags are call edges through the frontmatter import ────
python3 - "$TMP/map.xml" <<'PY'
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
rows = list(root.iter('s'))
def calls(node): return {c.get('n') for c in node.iter('c')}
by = {}
for s in rows:
    by.setdefault(s.get('n'), []).append(s)
layout = by['Layout'][0]
assert {'Header', 'formatTitle'} <= calls(layout), ('Layout should call Header (tag) and formatTitle (frontmatter)', calls(layout))
assert 'Fragment' not in calls(layout), 'the <Fragment> pseudo-tag became a call'
assert 'slot' not in calls(layout)
header = by['Header'][0]
assert 'Nav' in calls(header), ('Header should call Nav via <Nav client:load />', calls(header))
for page in by['index'] + by['404']:
    assert 'Layout' in calls(page), (page.get('id'), calls(page))
print('  PASS <Header/>, <Nav/>, <Layout> tags are call edges; Fragment and slot are not')
PY

# ── 4) the navigation verbs see the same edges (callers / uses / impact / deps) ───────────────────────
"$BIN" "$TMP/fix" --no-cache --callers=Layout > "$TMP/callers-layout.xml"
"$BIN" "$TMP/fix" --no-cache --callers=Nav    > "$TMP/callers-nav.xml"
"$BIN" "$TMP/fix" --no-cache --callers=helper > "$TMP/callers-helper.xml"
"$BIN" "$TMP/fix" --no-cache --uses=formatTitle > "$TMP/uses-formattitle.xml"
"$BIN" "$TMP/fix" --no-cache --uses=Layout    > "$TMP/uses-layout.xml"
"$BIN" "$TMP/fix" --no-cache --impact=helper  > "$TMP/impact-helper.xml"
"$BIN" "$TMP/fix" --no-cache --deps           > "$TMP/deps.xml"
python3 - "$TMP" <<'PY'
import sys, os, xml.etree.ElementTree as ET
t = sys.argv[1]
def paths(fn, tag='s'):
    return {e.get('p', '').split(':')[0] for e in ET.parse(os.path.join(t, fn)).getroot().iter(tag)}
cl = paths('callers-layout.xml')
assert {'pages/index.astro', 'pages/es/index.astro', 'pages/404.astro'} <= cl, ('--callers=Layout', cl)
assert 'components/Header.astro' in paths('callers-nav.xml'), ('--callers=Nav should name the .astro that renders it', paths('callers-nav.xml'))
assert 'components/Nav.tsx' in paths('callers-helper.xml'), 'the plain TS edge regressed'
uses = ET.parse(os.path.join(t, 'uses-formattitle.xml')).getroot()
sites = {(u.get('role'), u.get('p')) for u in uses.iter('u')}
assert ('call', 'layouts/Layout.astro:11') in sites, ('--uses=formatTitle should name the frontmatter call site with its REAL line', sites)
ul = ET.parse(os.path.join(t, 'uses-layout.xml')).getroot()
lsites = {u.get('p') for u in ul.iter('u')}
assert 'pages/blog/[...slug].astro:11' in lsites, ('the dynamic-route page still USES Layout at its tag line', lsites)
imp = open(os.path.join(t, 'impact-helper.xml'), encoding='utf-8').read()
for want in ('components/Nav.tsx', 'components/Header.astro', 'layouts/Layout.astro'):
    assert want in imp, ('--impact=helper should reach the .astro chain', want)
deps = ET.parse(os.path.join(t, 'deps.xml')).getroot()
incs = {}
for f in deps.iter('f'):
    incs[f.get('p')] = {i.get('t') for i in f.iter('inc')}
assert '../layouts/Layout.astro' in incs.get('pages/index.astro', set()), incs.get('pages/index.astro')
assert './Nav' in incs.get('components/Header.astro', set()), incs.get('components/Header.astro')
print('  PASS --callers/--uses/--impact/--deps agree with the map, with real file:line positions')
PY

# ── 5) bodies come from disk, VERBATIM (blanking never reaches the output) ────────────────────────────
"$BIN" "$TMP/fix" --no-cache --expand=Layout > "$TMP/expand.xml"
grep -q '<Header title={pageTitle} />' "$TMP/expand.xml" || { echo "  FAIL --expand=Layout lost the template text"; exit 1; }
grep -q 'rebeccapurple' "$TMP/expand.xml" || { echo "  FAIL --expand=Layout lost the <style> block"; exit 1; }
echo '  PASS --expand returns the verbatim .astro source'

# ── 6) control: remove the tag and the edge disappears (the mutation is asserted to have taken) ───────
python3 - "$TMP/fix/layouts/Layout.astro" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text()
assert '<Header title={pageTitle} />' in s
p.write_text(s.replace('<Header title={pageTitle} />', '<div>no header</div>'))
assert '<Header' not in p.read_text()
PY
"$BIN" "$TMP/fix" --no-cache > "$TMP/mut.xml"
python3 - "$TMP/mut.xml" <<'PY'
import sys, xml.etree.ElementTree as ET
by = {}
for s in ET.parse(sys.argv[1]).getroot().iter('s'):
    by.setdefault(s.get('n'), []).append(s)
assert 'Header' in by, 'Header component vanished with its tag (the def comes from the file, not the use)'
assert 'Header' not in {c.get('n') for c in by['Layout'][0].iter('c')}, 'edge survived the tag removal'
print('  PASS removing the tag removes the edge')
PY
cp "$ROOT/test/astrofix/layouts/Layout.astro" "$TMP/fix/layouts/Layout.astro"

# ── 7) determinism, warm==cold, G4 well-formedness, minification ─────────────────────────────────────
"$BIN" "$TMP/fix" --no-cache > "$TMP/a.xml"
cmp -s "$TMP/map.xml" "$TMP/a.xml" || { echo "  FAIL non-deterministic on an .astro corpus"; exit 1; }
for n in w1 w2 w3; do "$BIN" "$TMP/fix" > "$TMP/$n.xml"; done
cmp -s "$TMP/w1.xml" "$TMP/w2.xml" && cmp -s "$TMP/w2.xml" "$TMP/w3.xml" || { echo "  FAIL warm runs differ"; exit 1; }
cmp -s "$TMP/a.xml" "$TMP/w3.xml" || { echo "  FAIL warm .astro run differs from cold — cache/parserVer mismatch"; exit 1; }
xmllint --noout "$TMP/a.xml"
[ "$( grep -c '' "$TMP/a.xml" )" -le 1 ] || { echo "  FAIL newlines outside CDATA"; exit 1; }
echo '  PASS cold/warm determinism (x3), well-formed, minified'

# ── 8) doc/binary agreement: no new grammar, and the language lists name Astro ───────────────────────
# capture first: under pipefail a `grep -q` that exits on its first match SIGPIPEs the writer and fails the pipeline
"$BIN" --help > "$TMP/help.txt" 2>&1 || true
grep -q 'Astro' "$TMP/help.txt" || { echo "  FAIL --help does not mention Astro"; exit 1; }
grep -q 'Astro' "$ROOT/README.md" || { echo "  FAIL README does not mention Astro"; exit 1; }
PATH="$(cd "$(dirname "$BIN")" && pwd):$PATH" "$BIN" "$TMP/fix" --doctor > "$TMP/doctor.xml"
python3 - "$TMP/doctor.xml" <<'PY'
import sys, xml.etree.ElementTree as ET
rows = [c for c in ET.parse(sys.argv[1]).iter('c') if c.get('n') == 'grammars']
assert len(rows) == 1 and rows[0].get('loaded') == rows[0].get('expected'), rows[0].attrib
print('  PASS --help and README advertise Astro; doctor grammar census unchanged (rides typescript)')
PY

echo "astrocheck: ALL PASS"
