#include "session/setup_file.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace qfv {

namespace fs = std::filesystem;

namespace {

struct ParseError {
    int line;
    std::string msg;
};

/// Splits Tcl text into commands (lists of words) with their line numbers.
class Tokenizer {
public:
    Tokenizer(const std::string& text, std::map<std::string, std::string>& vars) : s(text), vars(vars) {}

    bool next(std::vector<std::string>& words, int& line) {
        words.clear();
        skipSpaceAndSeparators();
        if (i >= s.size())
            return false;
        line = lineNo;
        if (s[i] == '#') { // comment to end of line
            while (i < s.size() && s[i] != '\n')
                i++;
            return next(words, line);
        }
        while (i < s.size() && s[i] != '\n' && s[i] != ';') {
            if (s[i] == ' ' || s[i] == '\t' || s[i] == '\r') {
                i++;
                continue;
            }
            if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\n') { // continuation
                i += 2;
                lineNo++;
                continue;
            }
            words.push_back(word());
        }
        return true;
    }

private:
    void skipSpaceAndSeparators() {
        while (i < s.size() && (isspace(static_cast<unsigned char>(s[i])) || s[i] == ';')) {
            if (s[i] == '\n')
                lineNo++;
            i++;
        }
    }

    std::string word() {
        if (s[i] == '{') { // literal, nested braces
            int depth = 0;
            size_t start = ++i;
            for (; i < s.size(); i++) {
                if (s[i] == '\n')
                    lineNo++;
                if (s[i] == '{')
                    depth++;
                else if (s[i] == '}' && depth-- == 0)
                    break;
            }
            if (i >= s.size())
                throw ParseError{lineNo, "unterminated {"};
            return s.substr(start, i++ - start);
        }
        std::string out;
        bool quoted = s[i] == '"';
        if (quoted)
            i++;
        while (i < s.size()) {
            char c = s[i];
            if (quoted ? c == '"' : (c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '\r'))
                break;
            if (c == '$') {
                size_t start = ++i;
                bool braced = i < s.size() && s[i] == '{';
                if (braced)
                    start = ++i;
                while (i < s.size() && (braced ? s[i] != '}' : (isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')))
                    i++;
                std::string name = s.substr(start, i - start);
                if (braced)
                    i++;
                auto it = vars.find(name);
                if (it == vars.end())
                    throw ParseError{lineNo, "undefined variable $" + name};
                out += it->second;
                continue;
            }
            if (c == '[')
                throw ParseError{lineNo, "command substitution [...] is not supported"};
            if (c == '\\' && i + 1 < s.size()) {
                out += s[i + 1];
                i += 2;
                continue;
            }
            if (c == '\n')
                lineNo++;
            out += c;
            i++;
        }
        if (quoted) {
            if (i >= s.size())
                throw ParseError{lineNo, "unterminated \""};
            i++;
        }
        return out;
    }

    const std::string& s;
    std::map<std::string, std::string>& vars;
    size_t i = 0;
    int lineNo = 1;
};

double parseTime(const std::string& t, int line) {
    size_t pos = 0;
    double v = std::stod(t, &pos);
    std::string unit = t.substr(pos);
    if (unit.empty() || unit == "s")
        return v;
    if (unit == "m")
        return v * 60;
    if (unit == "h")
        return v * 3600;
    throw ParseError{line, "bad time '" + t + "' (use 30s, 10m, 1h)"};
}

} // namespace

bool parseSetupText(const std::string& text, const std::string& baseDir, const std::string& name,
                    SetupConfig& cfg, std::string& error) {
    std::map<std::string, std::string> vars;
    Tokenizer tok(text, vars);
    std::vector<std::string> w;
    int line = 0;
    auto resolve = [&](const std::string& f) {
        fs::path p(f);
        return (p.is_absolute() ? p : fs::path(baseDir) / p).lexically_normal().string();
    };
    try {
        while (tok.next(w, line)) {
            if (w.empty())
                continue;
            const std::string& cmd = w[0];
            auto need = [&](size_t n) {
                if (w.size() < n)
                    throw ParseError{line, cmd + ": missing argument"};
            };
            if (cmd == "set") {
                need(3);
                vars[w[1]] = w[2];
            }
            else if (cmd == "clear") {
                // `clear -all` starts a fresh setup in JasperGold; nothing to do here.
            }
            else if (cmd == "analyze") {
                for (size_t k = 1; k < w.size(); k++) {
                    auto& a = w[k];
                    if (a == "-sv" || a == "-sv09" || a == "-sv12" || a == "-sv17" || a == "-sv2012" ||
                        a == "-v2k" || a == "-verilog" || a == "-clear")
                        continue;
                    if (a == "-vhdl")
                        throw ParseError{line, "VHDL is not supported"};
                    if (a.rfind("+define+", 0) == 0) {
                        std::stringstream ss(a.substr(8));
                        for (std::string d; std::getline(ss, d, '+');)
                            if (!d.empty())
                                cfg.defines.push_back(d);
                    }
                    else if (a.rfind("+incdir+", 0) == 0) {
                        std::stringstream ss(a.substr(8));
                        for (std::string d; std::getline(ss, d, '+');)
                            if (!d.empty())
                                cfg.includeDirs.push_back(resolve(d));
                    }
                    else if (a == "-define") {
                        if (++k >= w.size())
                            throw ParseError{line, "-define needs a value"};
                        cfg.defines.push_back(w[k]);
                    }
                    else if (a == "-incdir") {
                        if (++k >= w.size())
                            throw ParseError{line, "-incdir needs a value"};
                        cfg.includeDirs.push_back(resolve(w[k]));
                    }
                    else if (!a.empty() && a[0] == '-') {
                        throw ParseError{line, "analyze: unsupported option " + a};
                    }
                    else {
                        cfg.files.push_back(resolve(a));
                    }
                }
            }
            else if (cmd == "elaborate") {
                for (size_t k = 1; k < w.size(); k++) {
                    if (w[k] == "-top" && k + 1 < w.size())
                        cfg.top = w[++k];
                    else if (w[k] == "-parameter" && k + 2 < w.size()) {
                        cfg.paramOverrides.push_back(w[k + 1] + "=" + w[k + 2]);
                        k += 2;
                    }
                    else
                        throw ParseError{line, "elaborate: unsupported option " + w[k]};
                }
            }
            else if (cmd == "clock") {
                need(2);
                if (w.size() > 2)
                    throw ParseError{line, "clock: only a single clock signal is supported"};
                cfg.clock = w[1];
            }
            else if (cmd == "reset") {
                need(2);
                if (w[1] == "-expression") {
                    need(3);
                    cfg.resetExpr = w[2];
                }
                else if (w[1][0] == '-') {
                    throw ParseError{line, "reset: unsupported option " + w[1]};
                }
                else {
                    cfg.resetExpr = w[1];
                }
            }
            else if (cmd == "assert" || cmd == "assume" || cmd == "cover") {
                SetupProperty p;
                p.kind = cmd;
                p.line = line;
                for (size_t k = 1; k < w.size(); k++) {
                    if (w[k] == "-name" && k + 1 < w.size())
                        p.name = w[++k];
                    else if (w[k][0] == '-')
                        throw ParseError{line, cmd + ": unsupported option " + w[k]};
                    else
                        p.text = w[k];
                }
                if (p.text.empty())
                    throw ParseError{line, cmd + ": missing property"};
                if (p.name.empty())
                    p.name = cmd + "_L" + std::to_string(line);
                cfg.properties.push_back(p);
            }
            else if (cmd == "set_prove_time_limit") {
                need(2);
                cfg.timeLimitSeconds = parseTime(w[1], line);
            }
            else if (cmd == "prove") {
                for (size_t k = 1; k < w.size(); k++)
                    if (w[k] == "-time_limit" && k + 1 < w.size())
                        cfg.timeLimitSeconds = parseTime(w[++k], line);
                cfg.prove = true;
            }
            else {
                throw ParseError{line, "unknown command '" + cmd +
                                           "' (QuickFV supports a JasperGold subset; see SPEC 7.2)"};
            }
        }
    }
    catch (const ParseError& e) {
        error = name + ":" + std::to_string(e.line) + ": " + e.msg;
        return false;
    }
    catch (const std::exception& e) {
        error = name + ":" + std::to_string(line) + ": " + e.what();
        return false;
    }
    if (cfg.top.empty()) {
        error = name + ": no 'elaborate -top <module>'";
        return false;
    }
    if (cfg.clock.empty()) {
        error = name + ": no 'clock <signal>'";
        return false;
    }
    return true;
}

bool parseSetupFile(const std::string& path, SetupConfig& cfg, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return parseSetupText(ss.str(), fs::path(path).parent_path().string(), path, cfg, error);
}

} // namespace qfv
