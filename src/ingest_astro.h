#pragma once

#if !defined( RIPWIRE_INGEST_TU )
#error "ingest_astro.h is a section of ingest.cpp; include it only there"
#endif

namespace rw
{
namespace
{

// ---- Astro (.astro): the region blanker, plus the two facts the TypeScript query cannot see ----
//
// An .astro file is three regions: a TypeScript FRONTMATTER between the first two `---` fence lines
// (imports, `interface Props`, helpers), an HTML TEMPLATE with `{expr}` interpolations and PascalCase
// component tags (`<Layout>`, `<Header />`), and zero or more client `<script>` blocks of TypeScript.
// `.astro` rides Lang::TypeScript + tree_sitter_typescript + the typescript tags.scm (kLangTable), the
// same reuse-a-vendored-grammar shape as Metal (C++) and CUDA (a C++ superset) — and, like those two,
// it was MEASURED before it was adopted (2026-09-07, 8 real .astro files from a production Astro 7
// site, docs/plans/astro-support.md): fed RAW to the TypeScript grammar all 8 parse degraded, a junk
// `const` symbol is minted, and 2 of the 8 lose every import row; with everything outside the
// frontmatter and <script> bodies replaced by spaces (newlines kept, so every byte offset and line
// number is still the file's own) all 8 parse clean, no junk, and every import survives. The blanked
// buffer is what the parser sees; the ORIGINAL bytes are what --expand and the lexical scorer see, so
// the blanking never reaches the output.
//
// tree-sitter-astro was considered and declined for this round: it extends tree-sitter-html (not
// vendored) and relies on TypeScript INJECTIONS for the frontmatter and every interpolation, which this
// query engine does not do — adopting it would mean two new grammars and still a second TS parse per
// file for the frontmatter. Its one advantage, seeing `{expr}` interpolations as code, is the pinned
// phase-1 non-goal below; a later round can measure whether it matters.
//
// The two facts the query cannot see, emitted by astroEmitFacts:
//   1. THE COMPONENT ITSELF — one SymKind::Class definition named by the file stem (`Layout.astro` is
//      imported and rendered as `Layout`; that is Astro's own convention), spanning the whole file, so
//      `--callers=Layout` / `--impact=Layout` have something to anchor on and every template-tag
//      reference below has an enclosing definition. nameByte = EOF for the same dedup reason
//      extractMarkdown gives (no identifier can START at EOF, so the identity never ties with a
//      frontmatter symbol at byte 0). Only an IDENTIFIER-LIKE stem earns the symbol
//      (astroIsIdentifierStem): `index`, `404`, `about-us` do, a dynamic route `[...slug]` does not —
//      pages are routes and nothing renders `<index />`, but a second `pages/es/index.astro` (i18n
//      routing) is a real, distinct page and keeps its own row under its own file.
//   2. TEMPLATE-TAG CALL REFERENCES — one RefRole::Call per `<PascalCase` tag in the template
//      (outside the TS regions, <style> bodies and HTML comments). The typescript tags.scm carries NO
//      JSX capture (measured: the same files as .tsx yield only role="import" sites for a layout), so
//      without this pass a component's render sites are invisible. The graph resolver binds the name
//      through the frontmatter import exactly as it binds any other TS call reference; the import
//      edge itself is already captured by the query on the blanked buffer. `Fragment` is Astro's
//      pseudo-element and never a symbol, so it is skipped; a dotted tag (`<Icon.Foo>`) takes its head.
//
// KNOWN OPEN FINDING, measured on the production site and NOT closed here (docs/plans/astro-support.md,
// phase 1.5): when a Markdown section WITH A BODY carries the same name as a component — the site's
// README has `## Layout` followed by prose — every template-tag reference to that component lands in
// graph_unresolved (`--callers=Layout` count=0, graph_unresolved=32 on the site; 3 on the fixture with
// a one-section NOTES.md added). Bisected 2026-09-07 on test/astrofix: a heading-ONLY section, a
// backtick mention in prose, and the same collision against a TypeScript `class Layout` all resolve;
// only a same-named section with a body against an Astro component symbol does not, in any file.
// The resolver's gauge says the defs were "all language-filtered", so the component row is losing
// its place in the name bucket to the section rather than tying with it. Root cause not isolated;
// the disclosure here is the evidence with its provenance, per the honesty contract.
//
// PHASE-1 NON-GOALS, pinned by test/astrocheck.sh so a later widening is a measured claim: template
// interpolations (`{title}`, `{items.map( … )}`) produce NO read/call edges; `Astro.props` / `Astro.url`
// stay unresolved externals like `React`; `<style>` bodies are blanked, never indexed (matching
// `.css` being unindexed). Two disclosures follow from measuring the blanked buffer: parse health
// (measureFileHealth) over-reports for .astro — mostly spaces, no ERROR nodes — and the size/verbosity
// lenses see a near-empty file. Both are floors, never wrong graph facts.
//
// Case-insensitive matching reuses docparse::detail::ciStartsWith (the HTML doc lane's own helper,
// already in this TU) rather than growing a fourth copy of the ASCII-fold loop.

struct AstroTagRef
{
    std::uint32_t    byte = 0;   // byte of the tag NAME (just past '<'), in the ORIGINAL file
    std::uint32_t    line = 0;   // 1-based
    std::string_view name;       // head segment of the tag name (`Icon.Foo` → `Icon`)
};

struct AstroRegion
{
    std::uint32_t begin = 0;
    std::uint32_t end   = 0;     // half-open
};

struct AstroScan
{
    std::uint32_t            size = 0;     // original file size (the component symbol's span and nameByte)
    std::vector<AstroRegion> tsRegions;    // frontmatter + kept <script> bodies: what the parser may see
    std::vector<AstroRegion> opaque;       // <style> bodies, HTML comments, dropped scripts: never scanned for tags
    std::vector<AstroTagRef> tags;         // PascalCase template tags, in byte order
};

/// True when a file stem can be a symbol name: [A-Za-z0-9_][A-Za-z0-9_-]*. A dynamic-route stem
/// (`[slug]`, `[...page]`) fails on its first byte; so does anything empty.
bool astroIsIdentifierStem( std::string_view stem ) noexcept
{
    if( stem.empty() )
    {
        return false;
    }
    for( std::size_t i = 0; i < stem.size(); ++i )
    {
        const unsigned char c = static_cast<unsigned char>( stem[ i ] );
        if( !( std::isalnum( c ) || c == '_' || ( i > 0 && c == '-' ) ) )
        {
            return false;
        }
    }
    return true;
}

/// True when a byte position falls inside one of the regions.
bool astroInside( std::size_t b, const std::vector<AstroRegion>& rs ) noexcept
{
    for( const AstroRegion& r : rs )
    {
        if( b >= r.begin && b < r.end )
        {
            return true;
        }
    }
    return false;
}

/// A <script> open tag's attribute text decides whether its body is TypeScript the parser should see.
/// Kept: no `type=` at all (Astro's default is a bundled TS module), or a module/javascript/typescript
/// type. Dropped: any other type (`application/ld+json` is the common one) and `set:html=` (the body
/// is a rendered expression, not code).
bool astroScriptBodyIsTs( std::string_view attrs ) noexcept
{
    if( docparse::detail::ciContains( attrs, "set:html" ) )
    {
        return false;
    }
    if( !docparse::detail::ciContains( attrs, "type=" ) )
    {
        return true;
    }
    // a `type=` is present: the ONLY kept values are module/javascript/typescript, and an attribute
    // list never carries one of those words for any other reason, so the containment test is the test.
    return docparse::detail::ciContains( attrs, "module" )
        || docparse::detail::ciContains( attrs, "javascript" )
        || docparse::detail::ciContains( attrs, "typescript" );
}

/// The `<script`/`<style` element opened at `lt`: file its body as a TS region (a kept script) or as
/// opaque (a style, a dropped script, a self-closing tag), and return where scanning resumes.
std::size_t astroElementBody( std::string_view src, std::size_t lt, bool isScript, AstroScan& out )
{
    const std::size_t tagEnd = src.find( '>', lt );
    if( tagEnd == std::string_view::npos )
    {
        return src.size();
    }
    if( src[ tagEnd - 1 ] == '/' )   // self-closing (`<script … set:html={…} />`): no body at all
    {
        out.opaque.push_back( { std::uint32_t( lt ), std::uint32_t( tagEnd + 1 ) } );
        return tagEnd + 1;
    }
    const std::string_view closeLit = isScript ? "</script" : "</style";
    std::size_t close = tagEnd + 1;   // the docparse HTML lane's own idiom for the closing tag
    while( close < src.size() && !docparse::detail::ciStartsWith( src, close, closeLit ) )
    {
        ++close;
    }
    const AstroRegion body{ std::uint32_t( tagEnd + 1 ), std::uint32_t( close ) };
    const bool keep = isScript && astroScriptBodyIsTs( src.substr( lt + 7, tagEnd - ( lt + 7 ) ) );
    ( keep ? out.tsRegions : out.opaque ).push_back( body );
    return close;
}

/// (1) The frontmatter: a `---` fence line at the very top (an optional UTF-8 BOM before it), closed by
/// the next line that is exactly `---`. Records the bytes BETWEEN the fence lines as a TS region and
/// returns the byte where the template begins (0 when there is no frontmatter).
std::size_t astroScanFrontmatter( std::string_view src, AstroScan& out )
{
    std::size_t pos = ( src.substr( 0, 3 ) == "\xEF\xBB\xBF" ) ? 3 : 0;
    if( src.substr( pos, 3 ) != "---" )
    {
        return 0;
    }
    const std::size_t openEnd = src.find( '\n', pos );
    if( openEnd == std::string_view::npos )
    {
        return 0;
    }
    for( std::size_t lineStart = openEnd + 1; lineStart < src.size(); )
    {
        std::size_t lineEnd = src.find( '\n', lineStart );
        lineEnd = ( lineEnd == std::string_view::npos ) ? src.size() : lineEnd;
        std::string_view line = src.substr( lineStart, lineEnd - lineStart );
        if( !line.empty() && line.back() == '\r' )
        {
            line.remove_suffix( 1 );
        }
        if( line == "---" )
        {
            out.tsRegions.push_back( { std::uint32_t( openEnd + 1 ), std::uint32_t( lineStart ) } );
            return lineEnd;
        }
        lineStart = lineEnd + 1;
    }
    return 0;
}

/// (2) <script> bodies that are TypeScript (TS regions), and <style> bodies, dropped scripts and HTML
/// comments (opaque: neither parsed nor scanned for tags), from the template start onward.
void astroScanBlocks( std::string_view src, std::size_t templateBegin, AstroScan& out )
{
    for( std::size_t at = templateBegin; at < src.size(); )
    {
        const std::size_t lt = src.find( '<', at );
        if( lt == std::string_view::npos )
        {
            return;
        }
        if( docparse::detail::ciStartsWith( src, lt, "<!--" ) )
        {
            std::size_t close = src.find( "-->", lt + 4 );
            close = ( close == std::string_view::npos ) ? src.size() : close + 3;
            out.opaque.push_back( { std::uint32_t( lt ), std::uint32_t( close ) } );
            at = close;
            continue;
        }
        const bool isScript = docparse::detail::ciStartsWith( src, lt, "<script" );
        const bool isStyle  = docparse::detail::ciStartsWith( src, lt, "<style" );
        at = ( isScript || isStyle ) ? astroElementBody( src, lt, isScript, out ) : lt + 1;
    }
}

/// (3) PascalCase tags in the template proper: outside every TS region and every opaque range.
void astroScanTags( std::string_view src, std::size_t templateBegin, AstroScan& out )
{
    std::uint32_t line = 1 + std::uint32_t( std::count( src.begin(), src.begin() + std::ptrdiff_t( templateBegin ), '\n' ) );
    for( std::size_t i = templateBegin; i < src.size(); ++i )
    {
        if( src[ i ] == '\n' )
        {
            ++line;
            continue;
        }
        if( src[ i ] != '<' || i + 1 >= src.size() || !std::isupper( static_cast<unsigned char>( src[ i + 1 ] ) )
            || astroInside( i, out.tsRegions ) || astroInside( i, out.opaque ) )
        {
            continue;
        }
        std::size_t end = i + 1;
        while( end < src.size() && ( std::isalnum( static_cast<unsigned char>( src[ end ] ) ) || src[ end ] == '_' || src[ end ] == '.' ) )
        {
            ++end;
        }
        const std::string_view name = src.substr( i + 1, end - ( i + 1 ) );
        const std::string_view head = name.substr( 0, name.find( '.' ) );
        if( head != "Fragment" )
        {
            out.tags.push_back( { std::uint32_t( i + 1 ), line, head } );
        }
    }
}

/// Locate the TypeScript regions and the PascalCase template tags of one .astro source. Pure, no parse.
AstroScan astroScan( std::string_view src )
{
    AstroScan out;
    out.size = std::uint32_t( src.size() );
    const std::size_t templateBegin = astroScanFrontmatter( src, out );
    astroScanBlocks( src, templateBegin, out );
    astroScanTags( src, templateBegin, out );
    return out;
}

/// Replace every byte outside the TS regions with a space, keeping newlines so offsets and lines hold.
void astroBlankOutsideTs( std::string& bytes, const AstroScan& scan ) noexcept
{
    std::size_t next = 0;   // regions are in byte order (frontmatter first, then scripts top-down)
    for( std::size_t i = 0; i < bytes.size(); ++i )
    {
        while( next < scan.tsRegions.size() && i >= scan.tsRegions[ next ].end )
        {
            ++next;
        }
        const bool kept = next < scan.tsRegions.size() && i >= scan.tsRegions[ next ].begin;
        if( !kept && bytes[ i ] != '\n' )
        {
            bytes[ i ] = ' ';
        }
    }
}

/// Emit the component/page definition (identifier-like stems only) and one call reference per tag.
void astroEmitFacts( std::uint32_t fileId, std::string_view stem, const AstroScan& scan,
                     std::vector<RawDef>& defs, std::vector<RawRef>& refs )
{
    if( astroIsIdentifierStem( stem ) )
    {
        RawDef d;
        d.fileId = fileId; d.line = 1; d.startByte = 0; d.endByte = scan.size;
        d.nameByte = scan.size; d.bodyByte = 0;
        d.kind = SymKind::Class; d.lang = Lang::TypeScript;
        d.name.assign( stem );
        defs.push_back( std::move( d ) );
    }
    for( const AstroTagRef& t : scan.tags )
    {
        RawRef r;
        r.fileId = fileId; r.startByte = t.byte; r.line = t.line; r.lang = Lang::TypeScript;
        r.role = RefRole::Call; r.isInherit = false;
        r.name.assign( t.name );
        refs.push_back( std::move( r ) );
    }
}

} // namespace
} // namespace rw
