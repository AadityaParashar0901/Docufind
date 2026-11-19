#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

struct Frequencies {
    int name = 0;
    int dirs = 0;
    int content = 0;
};

struct Document {
    std::string path;
    std::unordered_map<std::string, Frequencies> terms;
};

struct Posting {
    std::size_t docId;
    Frequencies tf;
};

struct Result {
    std::size_t docId;
    int tier; // 0 = filename, 1 = content, 2 = directory-only
    std::size_t filenameTerms;
    std::size_t contentTerms;
    std::size_t directoryTerms;
    std::size_t matchedTerms;
};

static std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::vector<std::string> tokenize(const std::string &text) {
    std::vector<std::string> out;
    std::string word;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c >= 128) word.push_back(static_cast<char>(std::tolower(c)));
        else if (!word.empty()) { out.push_back(word); word.clear(); }
    }
    if (!word.empty()) out.push_back(word);
    return out;
}

static bool isTextFile(const fs::path &p) {
    static const std::unordered_set<std::string> extensions = {
        ".txt", ".md", ".rst", ".csv", ".tsv", ".log", ".json", ".xml", ".html", ".htm",
        ".css", ".js", ".ts", ".jsx", ".tsx", ".c", ".h", ".cpp", ".hpp", ".cc", ".hh",
        ".py", ".java", ".kt", ".rs", ".go", ".rb", ".php", ".sql", ".yaml", ".yml", ".toml",
        ".ini", ".sh", ".bat", ".cmake", ".make", ".tex", ".conf"
    };
    return extensions.count(lower(p.extension().string())) != 0;
}

static std::string readFile(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string shellQuote(const std::string &s) {
    std::string q = "'";
    for (char c : s) {
        if (c == '\'') q += "'\\''";
        else q += c;
    }
    return q + "'";
}

static bool extractPdf(const fs::path &pdf, std::string &text) {
    // Optional dependency: Poppler's pdftotext command-line utility.
    const auto temp = fs::temp_directory_path() /
        ("docufind_pdf_" + std::to_string(std::hash<std::string>{}(pdf.string())) + ".txt");
    const std::string command = "pdftotext -layout " + shellQuote(pdf.string()) + " " + shellQuote(temp.string()) + " 2>/dev/null";
    if (std::system(command.c_str()) != 0) return false;
    text = readFile(temp);
    std::error_code ec;
    fs::remove(temp, ec);
    return true;
}

static void addTokens(Document &doc, const std::string &text, int field) {
    for (const auto &word : tokenize(text)) {
        auto &f = doc.terms[word];
        if (field == 0) ++f.name;
        else if (field == 1) ++f.dirs;
        else ++f.content;
    }
}

static bool saveIndex(const std::string &indexPath, const std::vector<Document> &docs) {
    std::ofstream out(indexPath, std::ios::trunc);
    if (!out) return false;
    out << "DOCUFIND_INDEX_1\n" << docs.size() << '\n';
    for (const auto &doc : docs) out << "DOC " << std::quoted(doc.path) << '\n';
    std::size_t termCount = 0;
    for (const auto &doc : docs) termCount += doc.terms.size();
    out << termCount << '\n';
    for (std::size_t id = 0; id < docs.size(); ++id) {
        for (const auto &[term, f] : docs[id].terms)
            out << "TERM " << std::quoted(term) << ' ' << id << ' ' << f.name << ' ' << f.dirs << ' ' << f.content << '\n';
    }
    return static_cast<bool>(out);
}

static bool loadIndex(const std::string &indexPath, std::vector<Document> &docs) {
    std::ifstream in(indexPath);
    if (!in) return false;
    std::string magic;
    std::getline(in, magic);
    if (magic != "DOCUFIND_INDEX_1") return false;
    std::size_t n = 0;
    if (!(in >> n) || n > 10000000) return false;
    docs.assign(n, Document{});
    for (std::size_t i = 0; i < n; ++i) {
        std::string tag;
        if (!(in >> tag) || tag != "DOC" || !(in >> std::quoted(docs[i].path))) return false;
    }
    std::size_t termCount = 0;
    if (!(in >> termCount) || termCount > 100000000) return false;
    for (std::size_t i = 0; i < termCount; ++i) {
        std::string tag, term;
        std::size_t id;
        Frequencies f;
        if (!(in >> tag >> std::quoted(term) >> id >> f.name >> f.dirs >> f.content) || tag != "TERM" || id >= docs.size()) return false;
        docs[id].terms.emplace(std::move(term), f);
    }
    return true;
}

static std::vector<Result> search(const std::vector<Document> &docs, const std::string &query, bool requireAll) {
    const auto words = tokenize(query);
    std::vector<Result> results;
    if (words.empty() || docs.empty()) return results;

    const std::unordered_set<std::string> unique(words.begin(), words.end());
    for (std::size_t id = 0; id < docs.size(); ++id) {
        std::size_t filenameTerms = 0;
        std::size_t contentTerms = 0;
        std::size_t directoryTerms = 0;
        std::size_t matched = 0;

        for (const auto &word : unique) {
            const auto it = docs[id].terms.find(word);
            if (it == docs[id].terms.end()) continue;
            ++matched;
            if (it->second.name > 0) ++filenameTerms;
            if (it->second.content > 0) ++contentTerms;
            if (it->second.dirs > 0) ++directoryTerms;
        }

        if (!matched || (requireAll && matched != unique.size())) continue;

        // Deliberate, explainable ranking: filename hits first, then content hits,
        // then documents that match only through directory names.
        const int tier = filenameTerms > 0 ? 0 : (contentTerms > 0 ? 1 : 2);
        results.push_back({id, tier, filenameTerms, contentTerms, directoryTerms, matched});
    }

    std::sort(results.begin(), results.end(), [&docs](const Result &a, const Result &b) {
        if (a.tier != b.tier) return a.tier < b.tier;
        if (a.filenameTerms != b.filenameTerms) return a.filenameTerms > b.filenameTerms;
        if (a.contentTerms != b.contentTerms) return a.contentTerms > b.contentTerms;
        if (a.matchedTerms != b.matchedTerms) return a.matchedTerms > b.matchedTerms;
        return docs[a.docId].path < docs[b.docId].path;
    });
    return results;
}

static std::size_t indexDirectory(const fs::path &root, const std::string &indexPath, std::vector<Document> &docs) {
    docs.clear();
    std::error_code ec;
    const fs::path absoluteRoot = fs::absolute(root, ec);
    if (ec || !fs::exists(absoluteRoot) || !fs::is_directory(absoluteRoot)) {
        std::cerr << "Error: directory does not exist or is not a directory.\n";
        return 0;
    }
    const fs::path absoluteIndex = fs::absolute(indexPath, ec);
    std::size_t skipped = 0;
    fs::recursive_directory_iterator it(absoluteRoot, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto p = it->path();
        if (!it->is_regular_file(ec)) continue;
        if (fs::absolute(p, ec) == absoluteIndex) continue;
        const auto ext = lower(p.extension().string());
        const bool pdf = ext == ".pdf";
        if (!pdf && !isTextFile(p)) continue;
        Document doc;
        doc.path = fs::relative(p, absoluteRoot, ec).generic_string();
        addTokens(doc, p.filename().string(), 0);
        addTokens(doc, p.parent_path().lexically_relative(absoluteRoot).generic_string(), 1);
        std::string content;
        if (pdf) {
            if (!extractPdf(p, content)) { ++skipped; continue; }
        } else content = readFile(p);
        addTokens(doc, content, 2);
        docs.push_back(std::move(doc));
    }
    if (skipped) std::cout << "Skipped " << skipped << " PDF(s): install Poppler's pdftotext to enable PDF indexing.\n";
    return docs.size();
}

static void printResults(const std::vector<Document> &docs, const std::vector<Result> &results) {
    if (results.empty()) { std::cout << "No matching documents found.\n"; return; }
    std::cout << "\nFound " << results.size() << " matching document(s):\n";
    const char *labels[] = {"FILENAME MATCH", "CONTENT MATCH", "DIRECTORY MATCH"};
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto &r = results[i];
        std::cout << std::setw(2) << (i + 1) << ". [" << labels[r.tier] << "] "
                  << docs[r.docId].path << "  [matched terms: " << r.matchedTerms
                  << ", filename: " << r.filenameTerms
                  << ", content: " << r.contentTerms
                  << ", directory: " << r.directoryTerms << "]\n";
    }
}

static void searchCommand(const std::string &indexPath, const std::string &query, bool requireAll) {
    std::vector<Document> docs;
    if (!loadIndex(indexPath, docs)) {
        std::cerr << "Could not load index: " << indexPath << "\nBuild an index first with --build.\n";
        std::exit(1);
    }
    const auto results = search(docs, query, requireAll);
    std::cout << "Index: " << indexPath << " (" << docs.size() << " documents)\n";
    std::cout << "Query: " << query << " [" << (requireAll ? "AND" : "OR") << "]\n";
    printResults(docs, results);
}

static void indexMode(const std::string &root, const std::string &indexPath) {
    std::vector<Document> docs;
    std::cout << "Indexing: " << root << "\n";
    const auto count = indexDirectory(root, indexPath, docs);
    if (count == 0) { std::cout << "No supported documents indexed.\n"; return; }
    if (!saveIndex(indexPath, docs)) { std::cerr << "Failed to write index: " << indexPath << "\n"; return; }
    std::size_t terms = 0;
    for (const auto &d : docs) terms += d.terms.size();
    std::cout << "Indexed " << count << " document(s), " << terms << " document-term entries.\n"
              << "Saved index to: " << indexPath << "\n";
}

int main(int argc, char **argv) {
    const bool wantsHelp = argc >= 2 && (std::string(argv[1]) == "help" ||
        std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h");
    if (argc < 4 || wantsHelp) {
        std::cout << "DocuFind: indexed document search\n\n"
                  << "Usage:\n"
                  << "  docufind <index-file> --build <directory>\n"
                  << "  docufind <index-file> --search <query> [--and]\n\n"
                  << "Options:\n"
                  << "  --build   Build/overwrite an index from a directory\n"
                  << "  --search  Search an existing index (OR mode by default)\n"
                  << "  --and     Require every query term (optional, with --search)\n\n"
                  << "Quote multi-word queries, e.g. \"machine learning\".\n";
        return wantsHelp ? 0 : 2;
    }

    const std::string indexPath = argv[1];
    const std::string option = argv[2];
    if (option == "--build" || option == "-b") {
        if (argc != 4) {
            std::cerr << "Usage: docufind <index-file> --build <directory>\n";
            return 2;
        }
        indexMode(argv[3], indexPath);
        return 0;
    }
    if (option == "--search" || option == "-s") {
        if (argc != 4 && !(argc == 5 && std::string(argv[4]) == "--and")) {
            std::cerr << "Usage: docufind <index-file> --search <query> [--and]\n";
            return 2;
        }
        searchCommand(indexPath, argv[3], argc == 5);
        return 0;
    }

    std::cerr << "Unknown option: " << option << "\nUse 'docufind --help' for usage.\n";
    return 2;
}
