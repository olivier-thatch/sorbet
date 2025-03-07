#include "core/packages/PackageDB.h"
#include "absl/strings/match.h"
#include "absl/types/span.h"
#include "common/sort/sort.h"
#include "core/AutocorrectSuggestion.h"
#include "core/GlobalState.h"
#include "core/Loc.h"

using namespace std;

namespace sorbet::core::packages {

namespace {
class NonePackage final : public PackageInfo {
public:
    MangledName mangledName() const {
        return MangledName();
    }

    absl::Span<const core::NameRef> fullName() const {
        notImplemented();
        return absl::Span<const core::NameRef>();
    }

    absl::Span<const string> pathPrefixes() const {
        notImplemented();
        return absl::Span<const string>();
    }

    unique_ptr<PackageInfo> deepCopy() const {
        notImplemented();
        return make_unique<NonePackage>();
    }

    Loc fullLoc() const {
        notImplemented();
        return Loc::none();
    }

    Loc declLoc() const {
        notImplemented();
        return Loc::none();
    }

    std::optional<core::AutocorrectSuggestion> addImport(const core::GlobalState &gs, const PackageInfo &pkg,
                                                         bool isTestImport) const {
        notImplemented();
        return nullopt;
    }

    std::optional<core::AutocorrectSuggestion> addExport(const core::GlobalState &gs,
                                                         const core::SymbolRef name) const {
        return {};
    }

    bool ownsSymbol(const core::GlobalState &gs, core::SymbolRef symbol) const {
        notImplemented();
        return false;
    }

    bool exportAll() const {
        notImplemented();
        return false;
    }

    bool visibleToTests() const {
        notImplemented();
        return false;
    }

    std::vector<std::vector<core::NameRef>> exports() const {
        return vector<vector<core::NameRef>>();
    }
    std::vector<std::vector<core::NameRef>> imports() const {
        return vector<vector<core::NameRef>>();
    }
    std::vector<std::vector<core::NameRef>> testImports() const {
        return vector<vector<core::NameRef>>();
    }
    std::vector<VisibleTo> visibleTo() const {
        return vector<VisibleTo>();
    }

    optional<pair<core::packages::StrictDependenciesLevel, core::LocOffsets>> strictDependenciesLevel() const {
        notImplemented();
        return nullopt;
    }

    optional<pair<core::NameRef, core::LocOffsets>> layer() const {
        notImplemented();
        return nullopt;
    }

    bool causesLayeringViolation(const core::packages::PackageDB &packageDB, const PackageInfo &otherPkg) const {
        notImplemented();
        return false;
    }

    core::packages::StrictDependenciesLevel minimumStrictDependenciesLevel() const {
        notImplemented();
        return core::packages::StrictDependenciesLevel::False;
    }

    std::optional<ImportType> importsPackage(MangledName mangledName) const {
        notImplemented();
        return nullopt;
    }

    ~NonePackage() {}

private:
    void notImplemented() const {
        ENFORCE(false, "Not implemented for NonePackage");
    }
};
static const NonePackage NONE_PKG;
} // namespace

UnfreezePackages::UnfreezePackages(PackageDB &db) : db(db) {
    ENFORCE(db.frozen);
    db.writerThread = this_thread::get_id();
    db.frozen = false;
}

UnfreezePackages::~UnfreezePackages() {
    ENFORCE(!db.frozen);
    fast_sort(db.mangledNames, [&db = this->db](auto a, auto b) -> bool {
        return PackageInfo::lexCmp(db.getPackageInfo(a).fullName(), db.getPackageInfo(b).fullName());
    });

    db.writerThread = std::thread::id();
    db.frozen = true;
}

MangledName PackageDB::enterPackage(unique_ptr<PackageInfo> pkg) {
    ENFORCE(!frozen);
    ENFORCE(writerThread == this_thread::get_id(), "PackageDB writes are not thread safe");
    auto nr = pkg->mangledName();
    auto [it, newlyInserted] = packages_.insert({nr, nullptr});
    if (newlyInserted) {
        for (const auto &prefix : pkg->pathPrefixes()) {
            packagesByPathPrefix[prefix] = nr;
        }
        mangledNames.emplace_back(nr);
    } else {
        // Package files do not have full featured content hashing. If the contents of one changes
        // we always run slow-path and fully rebuild the set of packages. In some cases, the LSP
        // fast-path may re-run on an unchanged package file. Sanity check to ensure the loc and
        // prefixes are the same.
        ENFORCE(it->second->declLoc() == pkg->declLoc());
        ENFORCE(it->second->pathPrefixes() == pkg->pathPrefixes());
    }
    it->second = move(pkg);
    ENFORCE(mangledNames.size() == packages_.size());
    return nr;
}

const MangledName PackageDB::getPackageNameForFile(FileRef file) const {
    if (this->packageForFile_.size() <= file.id()) {
        return MangledName();
    }

    return this->packageForFile_[file.id()];
}

void PackageDB::setPackageNameForFile(FileRef file, MangledName mangledName) {
    if (this->packageForFile_.size() <= file.id()) {
        this->packageForFile_.resize(file.id() + 1, MangledName());
    }

    this->packageForFile_[file.id()] = mangledName;
}

const PackageInfo &PackageDB::getPackageForFile(const core::GlobalState &gs, core::FileRef file) const {
    ENFORCE(frozen);

    // If we already have the package name cached, we can skip the slow path below. As this function is const, we cannot
    // update the vector if we fall back on the slow path.
    auto name = this->getPackageNameForFile(file);
    if (name.exists()) {
        return this->getPackageInfo(name);
    }

    // Note about safety: we're only using the file data for two pieces of information: the file path and the
    // sourceType. The path is present even on unloaded files, and the sourceType we're interested in is `Package`,
    // which will have been loaded by a previous step for the packageDB to be valid.
    //
    // See https://github.com/sorbet/sorbet/pull/5291 for more information.
    auto &fileData = file.dataAllowingUnsafe(gs);

    string_view path = fileData.path();
    int curPrefixPos = path.find_last_of('/');
    while (curPrefixPos > 0) {
        const auto &it = packagesByPathPrefix.find(path.substr(0, curPrefixPos + 1));
        if (it != packagesByPathPrefix.end()) {
            const auto &pkg = getPackageInfo(it->second);
            ENFORCE(pkg.exists());
            return pkg;
        }

        if (fileData.isPackage()) {
            // When looking up a `__package.rb` file do not search parent directories
            break;
        }
        curPrefixPos = path.find_last_of('/', curPrefixPos - 1);
    }
    return NONE_PKG;
}

const PackageInfo &PackageDB::getPackageInfo(const core::GlobalState &gs, std::string_view nameStr) const {
    auto cnst = core::packages::MangledName::mangledNameFromHuman(gs, nameStr);
    if (!cnst.exists()) {
        return NONE_PKG;
    }
    return getPackageInfo(cnst);
}

const PackageInfo &PackageDB::getPackageInfo(MangledName mangledName) const {
    auto it = packages_.find(mangledName);
    if (it == packages_.end()) {
        return NONE_PKG;
    }
    return *it->second;
}

PackageInfo *PackageDB::getPackageInfoNonConst(MangledName mangledName) {
    auto it = packages_.find(mangledName);
    if (it == packages_.end()) {
        return nullptr;
    }
    return it->second.get();
}

absl::Span<const MangledName> PackageDB::packages() const {
    return absl::MakeSpan(mangledNames);
}

absl::Span<const std::string> PackageDB::skipRBIExportEnforcementDirs() const {
    return absl::MakeSpan(skipRBIExportEnforcementDirs_);
}

absl::Span<const core::NameRef> PackageDB::layers() const {
    return absl::MakeSpan(layers_);
}

const int PackageDB::layerIndex(core::NameRef layer) const {
    auto findResult = absl::c_find(layers_, layer);
    ENFORCE(findResult != layers_.end());
    return std::distance(layers_.begin(), findResult);
}

const bool PackageDB::enforceLayering() const {
    return !layers_.empty();
}

absl::Span<const std::string> PackageDB::extraPackageFilesDirectoryUnderscorePrefixes() const {
    return absl::MakeSpan(extraPackageFilesDirectoryUnderscorePrefixes_);
}

absl::Span<const std::string> PackageDB::extraPackageFilesDirectorySlashDeprecatedPrefixes() const {
    return absl::MakeSpan(extraPackageFilesDirectorySlashDeprecatedPrefixes_);
}

absl::Span<const std::string> PackageDB::extraPackageFilesDirectorySlashPrefixes() const {
    return absl::MakeSpan(extraPackageFilesDirectorySlashPrefixes_);
}

const std::string_view PackageDB::errorHint() const {
    return errorHint_;
}

bool PackageDB::allowRelaxedPackagerChecksFor(MangledName mangledName) const {
    return absl::c_contains(allowRelaxedPackagerChecksFor_, mangledName);
}

PackageDB PackageDB::deepCopy() const {
    ENFORCE(frozen);
    auto result = this->emptyCopyWithOptions();

    result.packages_.reserve(this->packages_.size());
    for (auto const &[nr, pkgInfo] : this->packages_) {
        result.packages_[nr] = pkgInfo->deepCopy();
    }

    return result;
}

PackageDB PackageDB::emptyCopyWithOptions() const {
    ENFORCE(frozen);
    PackageDB result;
    result.enabled_ = this->enabled_;
    result.extraPackageFilesDirectoryUnderscorePrefixes_ = this->extraPackageFilesDirectoryUnderscorePrefixes_;
    result.extraPackageFilesDirectorySlashDeprecatedPrefixes_ =
        this->extraPackageFilesDirectorySlashDeprecatedPrefixes_;
    result.extraPackageFilesDirectorySlashPrefixes_ = this->extraPackageFilesDirectorySlashPrefixes_;
    result.packagesByPathPrefix = this->packagesByPathPrefix;
    result.mangledNames = this->mangledNames;
    result.errorHint_ = this->errorHint_;
    return result;
}

UnfreezePackages PackageDB::unfreeze() {
    return UnfreezePackages(*this);
}

} // namespace sorbet::core::packages
