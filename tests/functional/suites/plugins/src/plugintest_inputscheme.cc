#include "nix/fetchers/fetchers.hh"
#include "nix/util/file-system.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/types.hh"
#include "nix/util/url.hh"

namespace nix::fetchers {

/**
 * A fetcher for directories that contain a `.dummy` marker. It exists
 * to check that an out-of-tree `InputScheme` is a first-class citizen:
 * it must be able to claim a local repository, and the flake machinery
 * must route the resulting URL back to it.
 */
struct DummyInputScheme : InputScheme
{
    static constexpr std::string_view scheme = "dummy+file";

    std::optional<Input> inputFromURL(const ParsedURL & url, bool requireTree) const override
    {
        if (url.scheme != scheme)
            return std::nullopt;

        Input input{};
        input.attrs.insert_or_assign("type", std::string(scheme));
        input.attrs.insert_or_assign("path", urlPathToPath(url.path).string());
        return input;
    }

    std::optional<Input> inputFromAttrs(const Attrs & attrs) const override
    {
        getStrAttr(attrs, "path");
        Input input{};
        input.attrs = attrs;
        return input;
    }

    std::string_view schemeName() const override
    {
        return scheme;
    }

    std::string schemeDescription() const override
    {
        return "Dummy scheme for the plugin test suite.";
    }

    const std::map<std::string, AttributeInfo> & allowedAttrs() const override
    {
        static const std::map<std::string, AttributeInfo> attrs = {
            {"path", {}},
        };
        return attrs;
    }

    ParsedURL toURL(const Input & input) const override
    {
        return ParsedURL{
            .scheme = std::string(scheme),
            .authority = ParsedURL::Authority{},
            .path = pathToUrlPath(std::filesystem::path{getStrAttr(input.attrs, "path")}),
        };
    }

    std::optional<std::filesystem::path> getSourcePath(const Input & input) const override
    {
        return std::filesystem::path{getStrAttr(input.attrs, "path")};
    }

    bool isLocked(const Settings & settings, const Input & input) const override
    {
        return true;
    }

    std::optional<ParsedURL> localRepoURL(const std::filesystem::path & path) const override
    {
        if (!pathExists(path / ".dummy"))
            return std::nullopt;
        return ParsedURL{
            .scheme = std::string(scheme),
            .authority = ParsedURL::Authority{},
            .path = pathToUrlPath(path),
        };
    }

    std::pair<ref<SourceAccessor>, Input>
    getAccessor(const Settings & settings, Store & store, const Input & input) const override
    {
        return {makeFSSourceAccessor(std::filesystem::path{getStrAttr(input.attrs, "path")}), input};
    }
};

static auto rDummyInputScheme = OnStartup([] { registerInputScheme(std::make_unique<DummyInputScheme>()); });

} // namespace nix::fetchers
