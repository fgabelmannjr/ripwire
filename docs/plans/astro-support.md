<!-- Status ledger: update per phase. Phase 1 = region-sliced parse on the TS grammar (this branch). -->

| Phase | Status | Commit / note |
|---|---|---|
| 0. Plan committed to fork | done | this file |
| 1a. Fixture + gate written, observed RED | done | 42eda352 — exit 1 at section 1 on the pre-Astro binary |
| 1b. Implementation green on astrocheck | done | 49f3b45c — ALL PASS; quality-delta: 5 gating rows, all short-horizon-churn on the language tables/help text (acked with the suite result) |
| 1c. Full gates, sanitizer, determinism x3 | done | pargates 572 gates: 560 pass, 2 skip, 10 fail → all ten classified and closed in 17fd519f/10afcddf (stale stamp, mirror bump, gate count 557→558, docs index row, two re-pins, COMMANDS.md regen; rootrelemitcheck passes standalone; legendcoveragecheck fails identically on the pre-Astro binary, pre-existing). ASan: astrocheck ALL PASS, whole-tree clean. Determinism x3 byte-identical. Ack ledger: 7e7b85ba. |
| 1d. Docs, help, CHANGELOG, labeled ranking row | partial | README, --help, CHANGELOG, COMMANDS.md done. The labeled ranking row is DEFERRED: bench/recalleval labels bind to the pinned snapshot corpus (snapshot.lock), which carries no .astro; a row needs an Astro corpus admitted to the snapshot first — an upstream decision, not a fork's. |
| 1e. Upstream issue + PR | pending | |
| 1.5. Component symbol vs same-named Markdown section WITH a body | open | Repro: `cp -R test/astrofix /tmp/x; printf '## Layout\n\nprose\n' > /tmp/x/NOTES.md; ripwire /tmp/x --callers=Layout` → count=0, graph_unresolved=3. Heading-only, backtick mention, or a TS `class Layout` instead of the component: resolves. Gauge says defs were "all language-filtered". Suspects, in order: the model-build dedup/sort tie between a whole-file `cls` row (startByte=0, nameByte=EOF) and a section row, then the byName bucket build. Fix must be gate-first (add the NOTES.md arm to astrocheck, observe RED) and family-wide (any lane that emits a whole-file symbol). |
| 2. tree-sitter-astro lane (only if interpolation reads matter) | not started | |
| 3. Siblings: .vue / .svelte rows | not started | |

# Plan: Astro (`.astro`) support in ripwire

Derived 2026-09-07 from ripwire v0.4.0 source (`redhat-et/ripwire` @ main, clone in this
session's scratchpad), `docs/ARCHITECTURE.md` §1, `docs/METHODOLOGY.md` §1–§3, `CONTRIBUTING.md`,
`prompts/improve-for-my-language.md` (maintainers section), and probes against the real
Racecraft website (`landing-page/website/src`, 51 `.astro` files).

## Context

ripwire reports `.astro` as `unsupported-ext` (`ripwire . --skipped`), so every Astro component,
layout, and page is invisible to `--for`, `--callers`, `--impact`, `--deps`, and `--quality-delta`.
On the website that is 51 of ~165 source files, including every page and layout, so the map has
no entry points at all.

An `.astro` file is three regions:

1. **Frontmatter**: TypeScript between the first two `---` lines (imports, `interface Props`,
   consts, `Astro.props` destructuring, helper functions).
2. **Template**: HTML with `{expr}` interpolations and PascalCase component tags
   (`<Layout>`, `<SEO>`, `<MermaidDiagram>`).
3. **`<script>` blocks**: client-side TypeScript (11 of 51 website files; real functions such as
   `focusPrimaryCta`, `initRum` live here).

### What the docs constrain

- "Extraction is query-driven, never hand-rolled" (ARCHITECTURE §1): each language is a vendored
  grammar + `tags.scm` + one row in `kLangTable`. The one sanctioned exception is the Markdown DOC
  tier (`extractMarkdown`, a custom tree walk with query `""`), dispatched by a `Lang::Markdown`
  branch in `src/ingest_parsepool.h:438`.
- No grammar-injection support exists (`grep -rn inject src/` finds none). `tree-sitter-astro`
  (virchau13, MIT, extends `tree-sitter-html`) relies on TypeScript injections for frontmatter and
  every interpolation, and `tree-sitter-html` is not vendored. Adopting it means two new grammars,
  an external scanner, and still a second TS parse per file for the frontmatter.
- METHODOLOGY §1: write the gate first, prove it red against the pre-fix binary. §3: write the gate
  over the family, not one language. Siblings here are the single-file-component family: Vue
  (`<script setup lang="ts">`), Svelte (`<script lang="ts">`), MDX (`.mdx`, 3 on the website).
- A ranking claim needs a held-out labeled case in `bench/recalleval/` (`labels_ranking.tsv`,
  `query<TAB>primary<TAB>acceptable<TAB>class`), never a hand-inspected top-10.
- `kCacheVersion`/`parserVer` must move when the graph a cached blob describes changes (precedent:
  "37: +CUDA", `src/ingest_cache.h:640`).

### Measured (2026-09-07, 8 real website `.astro` files renamed `.ts`)

| Input to the TS grammar | `--skipped` `degraded-parse` | Files with import rows (`--deps`) | Import rows | Symbols |
|---|---|---|---|---|
| Raw file renamed `.ts` | 8 of 8 | 6 of 8 | 54 | junk `const`, `Props`, plus 2 functions from `<script>` |
| Raw file renamed `.tsx` (JSX grammar) | 8 of 8 | 6 of 8 | 54 | `Props` only; `--uses=Layout` sees 3 `role="import"` sites and no tag references |
| Template blanked to spaces, newlines kept | 0 of 8 | 8 of 8 | 62 | `Props` only, no junk |

Blanking is the Metal precedent (`kLangTable` comment: "a control experiment that blanked every
MSL-only keyword"), measured here to win on every axis. The two functions the raw parse found are
`<script>`-block functions, which argues for extracting that region on purpose rather than by
error recovery. The `.tsx` row also settles a design question: `queries/typescript/tags.scm` has
no JSX capture (`grep -n jsx` finds none), so a PascalCase component tag never becomes a reference
through the query; the template scan in step 3 is the only source of those edges.

Adjacent TS finding, not Astro-specific: the website's `@/*` tsconfig alias (`"@/*": ["./src/*"]`)
resolves 0 of 6 alias imports in single-root mode; `resolve.h` only reads `compilerOptions.paths`
for multi-root sibling evidence (§3.2). Report it separately; it caps `--deps` precision on any
aliased TS project.

## Recommended approach: region-sliced parse on the existing TypeScript grammar

No new grammar. `.astro` rides `Lang::TypeScript` + `tree_sitter_typescript` + the `typescript`
query, with a pre-parse that blanks everything outside the TypeScript regions (frontmatter and
`<script>` bodies) so byte offsets stay file-absolute and every existing TS behaviour (import
resolution, `--uses`, `--expand`, quality lenses) engages unchanged. A small post-pass adds the
two facts the TS grammar cannot see: the component definition and the template's component
references.

This is phase 1. A real `tree-sitter-astro` lane is phase 2, only if the labeled cases show that
template interpolation reads (`{t(...)}`, `{link.icon}`) matter for `--uses` precision.

### Changes, by file

1. **`src/ingest_crawl.h`**: add `{ ".astro", Lang::TypeScript, &tree_sitter_typescript, "typescript" }`
   to `kLangTable` and bump the exact extent (`std::array<LangEntry, 40>` → 41; the comment
   documents that the count is a compile-time guard).
2. **`src/ingest_parsepool.h`** (the `Lang::Markdown` branch at :438 is the template): before
   `parseTree`, when `le->ext == ".astro"`, run `astroBlankNonTs( bytes )` from a new
   `src/ingest_astro.h` section (registered in `src/ingest.cpp` like the other `ingest_*.h`
   sections, guarded by `RIPWIRE_INGEST_TU`). It keeps bytes inside `---…---` and inside
   `<script>…</script>` (not `is:inline` JSON-LD: skip `type="application/ld+json"` and
   `set:html`), replaces every other non-newline byte with a space. Parse the blanked buffer;
   measure `scan.health` on it.
3. **`src/ingest_astro.h`**, post-pass over the same file:
   - Emit one definition, `SymKind::Class`, name = file stem, `lang = Lang::TypeScript`, span =
     whole file, **only when the stem starts with an uppercase letter** (`Layout`, `PostCard`:
     Astro's component convention). Pages are routes, not components: nothing renders
     `<index />`, and the website has two `index.astro`, two `404.astro` (i18n), and three
     dynamic-route stems (`[slug]`, `[...page]`, `[...slug]`) that are not identifiers. A
     lowercase, digit-led, or bracket stem gets no component def; its frontmatter and scripts
     still index as ordinary TS. This is what `--callers=Layout` and `--impact=Layout` anchor on.
   - Scan the template region for `<[A-Z][A-Za-z0-9]*` (skip `<Fragment`) and emit
     `RefRole::Call` references named by the tag, one per site, at the tag's byte offset. The
     graph resolver binds them through the frontmatter import the same way it binds any TS
     call reference to an imported name; the import edge itself is already captured by the
     query. Dotted tags (`<Icon.Foo>`) take the head segment.
   - Client `<script>` bodies are already covered by the TS parse (step 2), so functions there
     are normal TS definitions.
4. **`src/resolve.h`**: add `".astro"` to `includeLangOf`'s `kExtLang` (`IncludeLang::Ts`) and to
   `resolveTsImport`'s `kFileExt` so `from './Header'` can land on `Header.astro` and an explicit
   `'./Header.astro'` specifier is a precise SameInclude hit.
5. **`src/lintrules.h:133`** and **`src/taskroute.h:323`**: add `.astro` to the TS extension lists
   so lint scoping and `--help-task` treat it as code.
6. **`src/ingest_cache.h`**: bump `parserVer` (record shape unchanged) with a "+Astro (.astro)"
   line in the version ledger, so a pre-Astro blob on an Astro-bearing tree is rejected.
7. **`src/verbs_doctor.h`** probe table: no new grammar, so no new row; add a comment that
   `.astro` rides the `typescript` probe (the `tsx`/`cuda` precedent).
8. **`src/cli.h:768`** help text and **`README.md:18`** language list: name Astro
   (`.astro — TypeScript frontmatter + <script>, component tags are call edges`).
9. **`CHANGELOG.md`** under "Added — languages", in the CUDA entry's style: what was measured
   before, what the fix captures, what stays out.

### Fixture and gate (write first, prove red)

`test/astrofix/`, the smallest tree that carries every construct the design rests on:

- `layouts/Layout.astro`: frontmatter with `interface Props`, an import of `Header.astro`, an
  arrow helper; template using `<Header />` and `<slot />`; a `<script>` block with a named
  function.
- `components/Header.astro`: imports a React island `Nav.tsx` (extension-less specifier) and
  renders `<Nav client:load />`.
- `components/Nav.tsx`: exports `Nav`, calls a helper from `lib/util.ts`.
- `pages/index.astro`: imports `Layout.astro` with the `.astro` extension, uses `<Layout>`, a
  `<Fragment>`, and a JSON-LD `is:inline` script that must NOT parse as TS.
- `lib/util.ts`: the helper.

`test/astrocheck.sh`, modeled on `test/cudacheck.sh` section for section:

1. `.astro` is crawled (`f p="…/Layout.astro"` present) and `--skipped` no longer lists `astro`.
2. Definitions: `Props`, the component symbols `Layout`/`Header`/`index`, the `<script>` function,
   and no junk (`const`, `slot`, `Fragment`, `script`, `html` never become symbol names).
3. **Acceptance**: `--callers=Layout` names `pages/index.astro`; `--callers=Header` names
   `Layout.astro`; `--callers=Nav` names `Header.astro` (tag → import → `.tsx`).
4. `--deps` shows `inc t="../layouts/Layout.astro"` from the page and the extension-less
   `./Nav` resolving to `Nav.tsx`; `--impact=util helper` reaches the `.astro` chain.
5. JSON-LD `is:inline` script contributes nothing; `--uses` of a frontmatter name reports a
   file:line inside the frontmatter.
6. Determinism (cold×2, warm×2, warm==cold), G4 well-formedness, minification, `--help` and
   README advertise Astro, `--quality-delta` clean on the fixture.
7. Non-goals pinned in the other direction: a lowercase HTML tag never mints a reference; a
   template interpolation `{helper()}` produces no edge in phase 1 (asserted absent, so a later
   phase-2 claim has a red gate waiting); `pages/index.astro`, a second `pages/es/index.astro`,
   `pages/404.astro`, and `pages/blog/[...slug].astro` mint no component symbol (add them to the
   fixture), while their frontmatter imports still appear in `--deps`.

Register `astrocheck` in `test/regression.sh`'s gate list; run
`python3 test/pargates.py . ./build/ripwire -j 6` plus the G1 sanitizer build before the PR
(CONTRIBUTING §1). Red-first: run `astrocheck.sh` against the pre-fix binary and record the
failing sections in the PR body.

### Labeled ranking case

Add to `bench/recalleval/labels_ranking.tsv` one row per lane on the website snapshot, for
example `which layout renders the 404 page<TAB>src/layouts/ErrorLayout.astro#ErrorLayout<TAB>src/pages/404.astro<TAB>astro`.
`run_recalleval.py --lane ranking` must move recall@5 up without raising pollution.

### Siblings (METHODOLOGY §3)

The region blanker is the shared mechanism. Design `astroBlankNonTs` as `sfcBlankNonTs( bytes,
regions )` where regions are per-extension rules, and land `.astro` alone in this PR with the
rule table shaped so `.vue` (`<script setup>` region, component def = stem) and `.svelte`
(`<script>` region) are one row and one fixture each in a follow-up. State in the PR that MDX is
deferred: its code regions are JSX inside Markdown, a different family.

### Explicit non-goals for phase 1

- Template interpolation reads and writes (`{title}`, `{items.map(...)}`): no edges. Disclosed
  in the CHANGELOG and asserted absent by the gate.
- `Astro.props`/`Astro.url` globals: unresolved external names, like `React` today.
- CSS `<style>` blocks: blanked, never indexed (matches `.css` being unindexed).
- `.mdx`: deferred.
- Two measurement side effects to disclose in the CHANGELOG: `scan.health` is measured on the
  blanked buffer, so parse health over-reports for `.astro` (mostly spaces, no ERROR nodes), and
  size/verbosity lenses see a near-empty file. Neither is wrong for the graph; both are floors,
  not totals, and the legend should say so.

## Verification (end to end)

1. `cmake -S . -B build && cmake --build build -j`; `test/astrocheck.sh` ALL PASS; the same under
   the ASAN build (`RIPWIRE_BIN=asan/ripwire`).
2. `python3 test/pargates.py . ./build/ripwire -j 6`: no regressions, in particular
   `tsimportprecisecheck`, `tsshapecheck`, `skippedcheck`, `langcensuscheck`, `doctorcheck`,
   `cachehashcheck`, `readmedriftcheck`, `versioncheck`.
3. On the website: `./build/ripwire website --skipped` no longer lists `astro`;
   `--callers=Layout` returns the 6 pages that import it by relative path (the 3 that import via
   the `@/` alias stay unresolved until the alias finding lands, so they are not a regression);
   `--impact=ErrorLayout` reaches the two relatively-imported error pages; map is byte-identical
   to the shipped binary on a tree with no `.astro` files (`ripwire-src` itself), proving the
   blast radius is zero outside the new extension.
4. `bench/recalleval/run_recalleval.py --lane ranking` on the labeled rows.
5. `./build/ripwire . --quality-delta` clean on the ripwire tree before the PR.

## Upstream hand-off

Open the issue first, per `prompts/improve-for-my-language.md`, titled
`TypeScript/Astro: .astro is unsupported-ext, 51 of 165 website files unindexed`, with the
measured table above and the `@/` alias finding as a separate item. Then the PR, red gate
output in the body, CHANGELOG entry in the house style.
