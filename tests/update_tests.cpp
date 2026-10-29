#include <medicon/update/Update.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace medicon::update;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

const std::string sha(64, 'a');

std::string manifest(const std::string& version = "1.0.5", const std::string& url_host = "medicon.example.com",
                     const std::string& extra = "") {
    return R"({"version":")" + version + R"(","channel":"stable","mcbe_version":"1.26.52","loader_api":1,)"
           R"("download_url":"https://)" + url_host + "/medicon/releases/" + version + R"(/MediconCore.dll",)"
           R"("sha256":")" + sha + R"(","size":4096,"changelog":["+ a","* b"])" + extra + "}";
}

void test_versions() {
    check(parse_version("1.0.5") == Version{1, 0, 5}, "plain version");
    check(parse_version("10.20.9999").has_value(), "four digit parts");
    for (const char* bad : {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.2.3 ", "-1.2.3", "1..3", "../1.2", "1.2.a", "12345.0.0"})
        check(!parse_version(bad), std::string("refused version '") + bad + "'");
    const std::vector<std::string> revisions{
        "1.0.1", "1.0.10", "1.0.2", "1.0.20", "1.0.25", "1.0.27", "1.0.28", "1.0.29", "1.0.3", "1.0.9"};
    for (std::size_t i = 0; i < revisions.size(); ++i) {
        for (std::size_t j = 0; j < revisions.size(); ++j) {
            const auto left = *parse_version(revisions[i]), right = *parse_version(revisions[j]);
            check((left < right) == (i < j) && (left == right) == (i == j) && (left > right) == (i > j),
                  "nested revision order: " + revisions[i] + " versus " + revisions[j]);
        }
    }
    check(*parse_version("1.2.0") < *parse_version("1.10.0") &&
          *parse_version("1.0.9999") < *parse_version("1.1.0") &&
          *parse_version("9.9.9") < *parse_version("10.0.0"), "major/minor versions keep numeric precedence");
    check(to_string({2, 0, 1}) == "2.0.1", "round trip");
}

void test_manifest() {
    std::string error;
    const auto parsed = parse_manifest(manifest(), "medicon.example.com", error);
    check(parsed && parsed->version == Version{1, 0, 5} && parsed->size == 4096 && parsed->changelog.size() == 2,
          "a valid manifest parses: " + error);
    check(!parse_manifest(manifest("1.0.5", "evil.example.com"), "medicon.example.com", error) &&
              error == "manifest_download_url",
          "another host is refused");
    std::string traversal = manifest();
    traversal.replace(traversal.find("releases/1.0.5/"), 15, "releases/1.0.5/../../");
    check(!parse_manifest(traversal, "medicon.example.com", error), "a path outside the release is refused");
    std::string http = manifest();
    http.replace(http.find("https://"), 8, "http://");
    check(!parse_manifest(http, "medicon.example.com", error), "plain http is refused");
    std::string api = manifest();
    api.replace(api.find("\"loader_api\":1"), 14, "\"loader_api\":2");
    check(!parse_manifest(api, "medicon.example.com", error) && error == "loader_api_mismatch", "another loader API");
    std::string big = manifest();
    big.replace(big.find("\"size\":4096"), 11, "\"size\":999999999999");
    check(!parse_manifest(big, "medicon.example.com", error), "an oversized Core is refused");
    std::string digest = manifest();
    digest.replace(digest.find(sha), 64, std::string(64, 'A'));
    check(!parse_manifest(digest, "medicon.example.com", error), "the digest must be lowercase hex");
    check(!parse_manifest("{not json", "medicon.example.com", error) && error == "manifest_json", "broken JSON");
    check(!parse_manifest(manifest("1.0"), "medicon.example.com", error), "a bad version");
    check(!parse_manifest(manifest(), "", error), "no allowed host means nothing is accepted");
}

void test_compatibility_and_digests() {
    check(mcbe_compatible("1.26.52", "1.26.52.3") && mcbe_compatible("1.26.52", "1.26.52"), "same game version");
    check(!mcbe_compatible("1.26.52", "1.26.520.1") && !mcbe_compatible("1.26.52", "1.26.51.2") &&
              !mcbe_compatible("bad", "bad"),
          "other game versions");
    check(same_digest(sha, sha) && !same_digest(sha, std::string(64, 'b')) && !same_digest("", ""), "digest comparison");
}

void test_state_and_order() {
    auto state = parse_state(R"({"good":["1.0.3","1.0.4","..\\x"],"bad":["1.0.6"],"last_run":"1.0.4"})");
    check(state.good.size() == 2 && state.bad.size() == 1 && state.last_run == "1.0.4", "state parses valid versions only");
    check(parse_state("garbage").good.empty() && parse_state("[]").last_run.empty(), "broken state is empty");
    const auto order = start_order({"1.0.3", "1.0.10", "1.0.6", "1.0.4", "1.0.4", "x"}, state);
    check((order == std::vector<std::string>{"1.0.4", "1.0.3", "1.0.10"}), "newest first, without failed versions");
    const auto nested = start_order({"1.0.27", "1.0.3", "1.0.25", "1.0.2", "1.0.28"}, {});
    check((nested == std::vector<std::string>{"1.0.3", "1.0.28", "1.0.27", "1.0.25", "1.0.2"}),
          "loader starts 1.0.3 before revisions inside 1.0.2");
    LoaderState nested_state;
    nested_state.good = {"1.0.2", "1.0.25", "1.0.27", "1.0.28", "1.0.3"};
    const auto stale = prune({"1.0.2", "1.0.25", "1.0.27", "1.0.28", "1.0.3"}, nested_state, "1.0.3");
    check((stale == std::vector<std::string>{"1.0.27", "1.0.25", "1.0.2"}), "pruning keeps the two newest good revisions, 1.0.3 and 1.0.28");
    mark_good(state, "1.0.6");
    check(state.bad.empty() && state.good.back() == "1.0.6", "a version that starts is no longer bad");
    mark_bad(state, "1.0.4");
    check(state.bad == std::vector<std::string>{"1.0.4"}, "a failing version is bad");
    check(state.bad_rule == 0, "state from an older loader has no bad rule");
    state.bad_rule = bad_rule_revision;
    const auto again = parse_state(serialize_state(state));
    check(again.good == state.good && again.bad == state.bad && again.last_run == state.last_run &&
              again.bad_rule == bad_rule_revision, "state round trip");
}

void test_prune() {
    LoaderState state;
    state.good = {"1.0.1", "1.0.2", "1.0.3"};
    const auto removed = prune({"1.0.1", "1.0.2", "1.0.3", "1.0.4", "1.0.0"}, state, "1.0.3");
    // Kept: 1.0.4 (newest, not started yet), 1.0.3 (running, good), 1.0.2 (good).
    check((removed == std::vector<std::string>{"1.0.1", "1.0.0"}), "prune keeps the newest, the running and two good");
    check(prune({"1.0.0"}, {}, "").empty(), "the only installed version stays");
}

} // namespace

int main() {
    try {
        test_versions();
        test_manifest();
        test_compatibility_and_digests();
        test_state_and_order();
        test_prune();
        std::cout << "Update tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Update test failed: " << error.what() << '\n';
        return 1;
    }
}
