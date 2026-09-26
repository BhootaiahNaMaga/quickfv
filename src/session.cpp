#include "session.h"

#include "slang/ast/Compilation.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/AllSyntax.h"

namespace qfv {

using namespace slang;

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
        .count();
}

Session::Session(SessionOptions opts) : opts(std::move(opts)) {
    for (auto& dir : this->opts.includeDirs)
        (void)sm.addUserDirectories(dir);
}

Bag Session::parseOptions() const {
    Bag bag;
    parsing::PreprocessorOptions pp;
    pp.predefines = opts.defines;
    bag.set(pp);
    return bag;
}

bool Session::load(std::string& error) {
    auto t0 = std::chrono::steady_clock::now();
    auto bag = parseOptions();
    trees.clear();
    for (auto& file : opts.files) {
        auto result = syntax::SyntaxTree::fromFile(file, sm, bag);
        if (!result) {
            error = "cannot read " + file;
            return false;
        }
        trees.push_back(*result);
    }
    parseMs_ = msSince(t0);
    return true;
}

std::unique_ptr<ast::Compilation> Session::compile(
    std::optional<size_t> replaceIndex, std::shared_ptr<syntax::SyntaxTree> replacement) {
    Bag bag = parseOptions();
    ast::CompilationOptions co;
    if (!opts.top.empty())
        co.topModules.emplace(opts.top);
    bag.set(co);
    auto comp = std::make_unique<ast::Compilation>(bag);
    for (size_t i = 0; i < trees.size(); i++)
        comp->addSyntaxTree(replaceIndex && *replaceIndex == i ? replacement : trees[i]);
    comp->getRoot(); // force elaboration
    return comp;
}

std::shared_ptr<syntax::SyntaxTree> Session::reparse(size_t index, std::string_view text) {
    auto path = std::string(sm.getRawFileName(trees[index]->root().sourceRange().start().buffer()));
    return syntax::SyntaxTree::fromText(text, sm, path, path, parseOptions());
}

slang::BufferID Session::buffer(size_t index) const {
    return trees[index]->root().sourceRange().start().buffer();
}

std::string_view Session::sourceText(size_t index) const {
    auto buf = buffer(index);
    auto text = sm.getSourceText(buf);
    if (!text.empty() && text.back() == '\0')
        text.remove_suffix(1);
    return text;
}

std::optional<std::pair<size_t, size_t>> Session::findModuleEnd(std::string_view name) const {
    for (size_t i = 0; i < trees.size(); i++) {
        auto& root = trees[i]->root();
        if (root.kind != syntax::SyntaxKind::CompilationUnit)
            continue;
        for (auto member : root.as<syntax::CompilationUnitSyntax>().members) {
            if (member->kind != syntax::SyntaxKind::ModuleDeclaration)
                continue;
            auto& mod = member->as<syntax::ModuleDeclarationSyntax>();
            if (mod.header->name.valueText() != name)
                continue;
            auto loc = mod.endmodule.location();
            if (sm.isMacroLoc(loc))
                return std::nullopt;
            return std::make_pair(i, size_t(loc.offset()));
        }
    }
    return std::nullopt;
}

} // namespace qfv
