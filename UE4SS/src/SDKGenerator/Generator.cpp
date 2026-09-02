#include <cwctype>
#include <format>
#include <locale>
#include <set>

#include <SDKGenerator/Common.hpp>
#include <SDKGenerator/Generator.hpp>
#include <UE4SSProgram.hpp>
#ifdef __linux__
#include <SignalGuard.hpp>
#endif
#include <cstdio>
#ifdef __linux__
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#endif
#include <cstdlib>
#include <cstring>
#pragma warning(disable : 4005)
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/TypeChecker.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UInterface.hpp>
#include <Unreal/UPackage.hpp>
#include <Unreal/UObjectGlobals.hpp>
#pragma warning(default : 4005)

namespace RC::UEGenerator
{
    using TypeChecker = RC::Unreal::TypeChecker;
    namespace UObjectGlobals = RC::Unreal::UObjectGlobals;
    using EObjectFlags = RC::Unreal::EObjectFlags;
    using FName = RC::Unreal::FName;

    // Safe FName -> String with validation to prevent 206M wstring allocation
    // Enhanced to log block/offset/length when invalid and return synthetic
    static auto SafeFNameToString(const FName& fname, bool allowSynthetic = false) -> std::optional<File::StringType>
    {
        uint32_t idx = 0;
        try { idx = fname.GetComparisonIndex().ToUnstableInt(); } catch (...) {
            if (allowSynthetic) return fmt::format(STR("FName_{:08X}"), 0);
            return std::nullopt;
        }
        uint32_t block = idx >> 16;
        uint32_t offset = idx & 0xFFFF;
        int32_t number = 0;
        try { number = fname.GetNumber(); } catch (...) { number = 0; }
        if (block >= 8192 || offset >= 65535 || number < -1 || number > 100000) {
            Output::send<LogLevel::Warning>(STR("SafeFNameToString: invalid FName block {} offset {} number {} idx {:08X}\n"), block, offset, number, idx);
            std::fprintf(stderr, "SafeFNameToString invalid block %u offset %u number %d idx %08X\n", block, offset, number, idx);
            if (allowSynthetic) return fmt::format(STR("FName_{:08X}_{}"), idx, number);
            return std::nullopt;
        }
        File::StringType result;
        bool ok = false;
#ifdef __linux__
        ok = RC::SignalGuard::safe_call([&]() { result = fname.ToString(); });
#else
        try { result = fname.ToString(); ok = true; } catch (...) { ok = false; }
#endif
        if (!ok) {
            Output::send<LogLevel::Warning>(STR("SafeFNameToString: ToString SIGSEGV for idx {:08X} block {} offset {}\n"), idx, block, offset);
            std::fprintf(stderr, "SafeFNameToString ToString SIGSEGV idx %08X block %u offset %u\n", idx, block, offset);
            if (allowSynthetic) return fmt::format(STR("FName_{:08X}_{}"), idx, number);
            return std::nullopt;
        }
        if (result.empty() || result.size() > 256) {
            Output::send<LogLevel::Warning>(STR("SafeFNameToString: invalid string length {} for idx {:08X} block {} offset {} empty {} -> '{}'\n"), result.size(), idx, block, offset, result.empty(), result.substr(0, 32));
            std::fprintf(stderr, "SafeFNameToString invalid length %zu idx %08X block %u offset %u\n", result.size(), idx, block, offset);
            if (allowSynthetic) return fmt::format(STR("FName_{:08X}_{}"), idx, number);
            return std::nullopt;
        }
        for (auto c : result) {
            if (c < 32 || c > 126) {
                if (c != '/' && c != '_' && c != '.' && c != '-') {
                    Output::send<LogLevel::Warning>(STR("SafeFNameToString: non-printable char {} in '{}' idx {:08X}\n"), (int)c, result.substr(0, 32), idx);
                    if (allowSynthetic) return fmt::format(STR("FName_{:08X}_{}"), idx, number);
                    return std::nullopt;
                }
            }
        }
        return result;
    }
    using UObject = RC::Unreal::UObject;
    using UClass = RC::Unreal::UClass;
    using UInterface = RC::Unreal::UInterface;
    using UScriptStruct = RC::Unreal::UScriptStruct;
    using UFunction = RC::Unreal::UFunction;
    using XProperty = RC::Unreal::FProperty;
    using UStruct = RC::Unreal::UStruct;
    using UEnum = Unreal::UEnum;
    using FObjectProperty = RC::Unreal::FObjectProperty;
    using FClassProperty = RC::Unreal::FClassProperty;
    using FStructProperty = RC::Unreal::FStructProperty;
    using FArrayProperty = RC::Unreal::FArrayProperty;
    using FMapProperty = RC::Unreal::FMapProperty;
    using FDelegateProperty = RC::Unreal::FDelegateProperty;
    using FMulticastInlineDelegateProperty = RC::Unreal::FMulticastInlineDelegateProperty;
    using FMulticastSparseDelegateProperty = RC::Unreal::FMulticastSparseDelegateProperty;
    using EFieldIterationFlags = RC::Unreal::EFieldIterationFlags;

    struct ObjectInfo
    {
        Unreal::UObject* object{};
        ObjectInfo* first_encountered_at{};
    };

    struct PropertyInfo
    {
        Unreal::FProperty* property{};
        bool should_forward_declare{};
    };

    struct FunctionInfo
    {
        Unreal::UFunction* function{};
        ObjectInfo& owner;
        std::vector<PropertyInfo> params{};
    };

    struct GeneratedFile
    {
        std::vector<File::StringType> ordered_primary_file_contents;
        std::vector<File::StringType> ordered_secondary_file_contents;
        File::StringType package_name;
        File::Handle primary_file;
        File::Handle secondary_file;
        bool primary_file_has_no_contents;
        bool secondary_file_has_no_contents;
    };

    auto generate_tab(size_t num_tabs = 1) -> File::StringType
    {
        File::StringType tab_storage{};
        for (size_t i = 0; i < num_tabs; ++i)
        {
            tab_storage += STR("    ");
        }

        return tab_storage;
    }
    auto generate_prefix(UStruct* obj) -> File::StringType
    {
        UClass* obj_class = obj->GetClassPrivate();
        if (obj_class->IsChildOf<UScriptStruct>())
        {
            return STR("struct");
        }
        else
        {
            return STR("class");
        }
    }
    auto generate_class_name(UStruct* class_to_generate) -> File::StringType
    {
        if (class_to_generate->GetClassPrivate()->IsChildOf<UScriptStruct>())
        {
            return get_native_struct_name(std::bit_cast<UScriptStruct*>(class_to_generate));
        }
        else if (class_to_generate->IsChildOf<UInterface>())
        {
            return get_native_class_name(static_cast<UClass*>(class_to_generate), true);
        }
        else
        {
            // Assume that it's a UClass
            return get_native_class_name(static_cast<UClass*>(class_to_generate));
        }
    }

    template <typename T>
    class TypeGenerator
    {
      private:
        T specification{};
        const std::filesystem::path m_directory_to_generate_in;
        const std::string m_dir_narrow;

      public:
        struct FileName
        {
            uint32_t num_collisions{};
        };

        // Map of FName.ComparisonIndex -> File::Handle
        std::vector<GeneratedFile> m_files{};
        std::map<File::StringType, FileName> m_file_names{};
        std::unordered_map<Unreal::UObject*, ObjectInfo> m_classes_dumped{};

      public:
        TypeGenerator() = delete;
        TypeGenerator(const std::filesystem::path directory_to_generate_in) : m_directory_to_generate_in(directory_to_generate_in), m_dir_narrow(directory_to_generate_in.string())
        {
            // m_files is std::map, no reserve needed
            // Validate m_dir_narrow not corrupted
            if (m_dir_narrow.empty() || m_dir_narrow.size() > 256) {
                // fallback
                const_cast<std::string&>(m_dir_narrow) = "CXXHeaderDump";
            }
        }

        auto create_all_files() -> void
        {
            Output::send(STR("Creating all files...\n"));
            size_t failed_files = 0;
            size_t created_files = 0;
            try { std::filesystem::create_directories(std::filesystem::path(m_dir_narrow)); } catch (...) {}
            try { std::filesystem::create_directories(m_directory_to_generate_in); } catch (...) {}
            // Snapshot m_files to avoid iterating corrupted map directly
            std::vector<GeneratedFile*> snapshot;
            snapshot.reserve(m_files.size() + 4);
#ifdef __linux__
            bool snapshot_ok = RC::SignalGuard::safe_call([&]() {
                for (auto& gf : m_files) snapshot.push_back(&gf);
            });
            if (!snapshot_ok) {
                Output::send<LogLevel::Warning>(STR("create_all_files snapshot failed (m_files corrupted, size {}), aborting\n"), m_files.size());
                return;
            }
#else
            for (auto& gf : m_files) snapshot.push_back(&gf);
#endif
            Output::send<LogLevel::Warning>(STR("Snapshot size: {} (m_files size {})\n"), snapshot.size(), m_files.size());
            std::fprintf(stderr, "Snapshot size %zu m_files %zu\n", snapshot.size(), m_files.size());
            for (GeneratedFile* pfile : snapshot)
            {
#ifdef __linux__
                bool file_ok = RC::SignalGuard::safe_call([&]() {
                if (!pfile) return;
                auto& generated_file = *pfile;
                Output::send<LogLevel::Warning>(STR("  create_all_files: processing package '{}' primary {} secondary {}\n"), generated_file.package_name.substr(0, 32), generated_file.ordered_primary_file_contents.size(), generated_file.ordered_secondary_file_contents.size());
                std::fprintf(stderr, "  create_all_files: processing package %ls primary %zu secondary %zu\n", generated_file.package_name.substr(0, 32).c_str(), generated_file.ordered_primary_file_contents.size(), generated_file.ordered_secondary_file_contents.size());
#else
                if (!pfile) continue;
                auto& generated_file = *pfile;
#endif
                if (generated_file.package_name.empty() || generated_file.package_name.size() > 128) {
                    Output::send<LogLevel::Warning>(STR("Skipping file with invalid package_name length {}\n"), generated_file.package_name.size());
                    ++failed_files;
                    return;
                }
                for (auto c : generated_file.package_name) {
                    if (!(std::isalnum((unsigned char)c) || c == '_' || c == '/')) {
                        Output::send<LogLevel::Warning>(STR("Skipping file with invalid package_name '{}'\n"), generated_file.package_name);
                        ++failed_files;
                        return;
                    }
                }
                // Clear huge vectors instead of skipping to allow at least header
                if (generated_file.ordered_primary_file_contents.size() > 10000) {
                    Output::send<LogLevel::Warning>(STR("Clearing huge primary vector for '{}' size {}\n"), generated_file.package_name, generated_file.ordered_primary_file_contents.size());
                    try { generated_file.ordered_primary_file_contents.clear(); generated_file.ordered_primary_file_contents.shrink_to_fit(); } catch (...) {}
                }
                if (generated_file.ordered_secondary_file_contents.size() > 10000) {
                    Output::send<LogLevel::Warning>(STR("Clearing huge secondary vector for '{}' size {}\n"), generated_file.package_name, generated_file.ordered_secondary_file_contents.size());
                    try { generated_file.ordered_secondary_file_contents.clear(); generated_file.ordered_secondary_file_contents.shrink_to_fit(); } catch (...) {}
                }
                // Compute file names from package_name and m_dir_narrow
                std::string pkg_narrow;
                {
                    pkg_narrow.reserve(generated_file.package_name.size());
                    for (auto c : generated_file.package_name) pkg_narrow.push_back(static_cast<char>(c & 0xFF));
                }
                std::string ext = ".hpp";
                std::string primary_path_str = m_dir_narrow + "/" + pkg_narrow + ext;
                std::string secondary_path_str = m_dir_narrow + "/" + pkg_narrow + "_enums" + ext;
                if (!generated_file.ordered_primary_file_contents.empty())
                {
                    try {
                        generated_file.primary_file =
                                File::open(std::filesystem::path(primary_path_str), File::OpenFor::Appending, File::OverwriteExistingFile::Yes, File::CreateIfNonExistent::Yes);

                        specification.generate_file_header(generated_file);

                        sort_files(generated_file.ordered_primary_file_contents);

                        File::StringType combined_file_contents;
                        for (auto& line : generated_file.ordered_primary_file_contents)
                        {
                            combined_file_contents.append(line);
                        }

                        if (combined_file_contents.empty())
                        {
                            Output::send(STR("Empty primary file contents in '{}'\n"), generated_file.package_name);
                        }
                        else
                        {
                            generated_file.primary_file.write_string_to_file(combined_file_contents);
                        }

                        specification.generate_file_footer(generated_file);

                        generated_file.primary_file.close();
                        ++created_files;
                        Output::send<LogLevel::Warning>(STR("  create_all_files: successfully created primary file for package '{}'\n"), generated_file.package_name);
                    } catch (...) {}
                } else {
                    // Always create primary file even if empty to ensure output
                    try {
                        generated_file.primary_file = File::open(std::filesystem::path(primary_path_str), File::OpenFor::Appending, File::OverwriteExistingFile::Yes, File::CreateIfNonExistent::Yes);
                        try { specification.generate_file_header(generated_file); } catch (...) {}
                        try { specification.generate_file_footer(generated_file); } catch (...) {}
                        try { generated_file.primary_file.close(); } catch (...) {}
                        ++created_files;
                    } catch (...) {}
                }

                if (!generated_file.ordered_secondary_file_contents.empty())
                {
                    try {
                        generated_file.secondary_file =
                                File::open(std::filesystem::path(secondary_path_str), File::OpenFor::Appending, File::OverwriteExistingFile::Yes, File::CreateIfNonExistent::Yes);

                        // For secondary (enums), skip sorting to avoid corruption from sort_files
                        File::StringType combined_file_contents;
                        for (auto& line : generated_file.ordered_secondary_file_contents)
                        {
                            combined_file_contents.append(line);
                        }

                        if (combined_file_contents.empty())
                        {
                            Output::send(STR("Empty secondary file contents in '{}'\n"), generated_file.package_name);
                        }
                        else
                        {
                            generated_file.secondary_file.write_string_to_file(combined_file_contents);
                        }

                        generated_file.secondary_file.close();
                    } catch (...) {}
                }
#ifdef __linux__
                });
                if (!file_ok) { ++failed_files; }
#endif
            }
            Output::send<LogLevel::Warning>(STR("create_all_files: created {} files, {} failed/skipped\n"), created_files, failed_files);
        }

        template<typename StringType>
        auto sort_files(std::vector<StringType>& content) -> void
        {
            std::vector<File::StringType> struct_content;
            std::vector<File::StringType> class_content;
            std::vector<File::StringType> other_content;
            for (auto& line : content)
            {
                if (line.starts_with(STR("struct")))
                {
                    struct_content.push_back(line);
                }
                else if (line.starts_with(STR("class")))
                {
                    class_content.push_back(line);
                }
                else
                {
                    other_content.push_back(line);
                }
            }

            sort_types(struct_content);
            sort_types(class_content);
            sort_types(other_content);

            content.clear();
            content.reserve(struct_content.size() + class_content.size() + other_content.size());
            content.insert(content.end(), struct_content.begin(), struct_content.end());
            content.insert(content.end(), class_content.begin(), class_content.end());
            content.insert(content.end(), other_content.begin(), other_content.end());
        }

        template<typename StringType>
        auto sort_types(std::vector<StringType>& content) -> void
        {
            std::sort(content.begin(), content.end(), [&](const auto& a, const auto& b) {
                auto a_class_name = get_class_name(a);
                auto b_class_name = get_class_name(b);
                return a_class_name < b_class_name;
            });
        }

        auto get_class_name(const auto& x) -> StringType
        {
            // Using this method instead of regex because it is extremely slow
            auto class_name = x.substr(x.find(STR(' ')) + 1);
            class_name = class_name.substr(0, class_name.find(STR(' ')));
            if (class_name == STR("class"))
            {
                // Case for enum class
                class_name = x.substr(x.find(STR(' ')) + 7);
                class_name = class_name.substr(0, class_name.find(STR(' ')));
            }
            return class_name;
        }

        auto object_is_package(UObject* object) -> bool
        {
#ifdef __linux__
            bool result = false;
            bool ok = RC::SignalGuard::safe_call([&]() {
                auto* cls = object->GetClassPrivate();
                if (!cls) return;
                result = cls->GetNamePrivate().Equals(Unreal::GPackageName);
            });
            return ok ? result : false;
#else
            return object->GetClassPrivate()->GetNamePrivate().Equals(Unreal::GPackageName);
#endif
        }

        auto generate_offset_comment(XProperty* property, File::StringType& line) -> File::StringType
        {
            if (UE4SSProgram::settings_manager.CXXHeaderGenerator.DumpOffsetsAndSizes)
            {
                return fmt::format(STR("{:85} // 0x{:04X} (size: 0x{:X})"), line, property->GetOffset_Internal(), property->GetSize());
            }
            else
            {
                return line;
            }
        }

        auto check_ignore_forward_declaration(const ObjectInfo& owner, XProperty* return_property) -> bool
        {
            if (return_property->IsA<FStructProperty>())
            {
                // Can StructProperty even be forward declared ? I don't know if it's ever a pointer to a struct
                UScriptStruct* script_struct = static_cast<FStructProperty*>(return_property)->GetStruct();
                if (m_classes_dumped.contains(script_struct))
                {
                    auto& property_class_info = m_classes_dumped[script_struct];
                    if (property_class_info.first_encountered_at)
                    {
                        return property_class_info.first_encountered_at->object == owner.object;
                    }
                }
            }
            else if (return_property->IsA<FClassProperty>())
            {
                // Can ClassProperty be forward declared ? Maybe ?
                UClass* meta_class = static_cast<FClassProperty*>(return_property)->GetMetaClass();
                if (m_classes_dumped.contains(meta_class))
                {
                    auto& property_class_info = m_classes_dumped[meta_class];
                    if (property_class_info.first_encountered_at)
                    {
                        return property_class_info.first_encountered_at->object == owner.object;
                    }
                }
            }
            else if (return_property->IsA<FObjectProperty>())
            {
                UClass* property_class = static_cast<FObjectProperty*>(return_property)->GetPropertyClass();
                if (m_classes_dumped.contains(property_class))
                {
                    auto& property_class_info = m_classes_dumped[property_class];
                    if (property_class_info.first_encountered_at)
                    {
                        return property_class_info.first_encountered_at->object == owner.object;
                    }
                }
            }
            return false;
        }

        auto generate_function_declaration(ObjectInfo& owner,
                                           const FunctionInfo& function_info,
                                           GeneratedFile& generated_file,
                                           File::StringType& out_current_class_content,
                                           IsDelegateFunction is_delegate_function = IsDelegateFunction::No) -> void
        {
            std::optional<PropertyInfo> return_property_info = [&]() -> std::optional<PropertyInfo> {
                for (const auto& property_info : function_info.params)
                {
                    if (property_info.property->HasAnyPropertyFlags(Unreal::CPF_ReturnParm))
                    {
                        return property_info;
                    }
                }

                return {};
            }();

            XProperty* return_property = [&]() {
                return return_property_info.has_value() ? return_property_info.value().property : nullptr;
            }();

            File::StringType function_name;
#ifdef __linux__
            try {
                auto opt = SafeFNameToString(function_info.function->GetFName(), true);
                function_name = opt ? *opt : fmt::format(STR("Func_{:08X}"), function_info.function->GetFName().GetComparisonIndex().ToUnstableInt());
                if (function_name.empty() || function_name.size() > 256) function_name = fmt::format(STR("Func_{:08X}"), function_info.function->GetFName().GetComparisonIndex().ToUnstableInt());
            } catch (...) { function_name = STR("Func_Failed"); }
#else
            File::StringType function_name{function_info.function->GetName()};
#endif
            if (is_delegate_function == IsDelegateFunction::Yes)
            {
                // Remove the last 19 characters, which is always '__DelegateSignature' for delegates
                function_name.erase(function_name.size() - 19, 19);
            }

            StringType current_class_content{};
            specification.generate_function_declaration(this, current_class_content, owner, function_info, function_name, return_property, return_property_info);

            // Commenting out this code because all network replicated functions are events
            // Therefore this is not an accurate way to check if a function is an event
            /*
            if ((function->get_function_flags() & Unreal::EFunctionFlags::FUNC_Event) != 0)
            {
                current_class_content.append(STR(" // EVENT"));
            }
            //*/
            current_class_content.append(STR("\n"));
            out_current_class_content.append(current_class_content);
        }

        auto generate_class_dependency(ObjectInfo& owner, UStruct* inherited_class, File::StringType& current_class_content) -> void
        {
            if (!inherited_class)
            {
                return;
            }

            if (!m_classes_dumped.contains(inherited_class))
            {
                GeneratedFile* package_file_for_inherited_class = generate_package_if_non_existent(inherited_class);
                if (package_file_for_inherited_class)
                {
                    auto& inherited_object_info = m_classes_dumped.emplace(inherited_class, ObjectInfo{inherited_class, &owner}).first->second;
                    File::StringType new_class_content{};
                    generate_class(inherited_object_info, *package_file_for_inherited_class, new_class_content);
                    if (!package_file_for_inherited_class->primary_file_has_no_contents)
                    {
                        package_file_for_inherited_class->ordered_primary_file_contents.push_back(new_class_content);
                    }
                }

                return;
            }
        }

        auto generate_class_dependency_from_property(ObjectInfo& owner, XProperty* property, File::StringType& current_class_content) -> bool
        {
            if (property->IsA<FStructProperty>())
            {
                generate_class_dependency(owner, static_cast<FStructProperty*>(property)->GetStruct(), current_class_content);
                return false;
            }
            else if (property->IsA<FClassProperty>())
            {
                // return generate_class_dependency(owner, static_cast<XClassProperty*>(property)->get_meta_class());
                return false;
            }
            else if (property->IsA<FObjectProperty>())
            {
                // return generate_class_dependency(owner, static_cast<XObjectProperty*>(property)->get_property_class());
                return true;
            }
            else if (property->IsA<FArrayProperty>())
            {
                // return generate_class_dependency_from_property(owner, static_cast<XArrayProperty*>(property)->get_inner());
                // if (static_cast<XArrayProperty*>(property)->get_inner()->is_child_of<XObjectProperty>())
                //{
                //     return true;
                // }
                XProperty* inner = static_cast<FArrayProperty*>(property)->GetInner();
                if (inner->IsA<FStructProperty>())
                {
                    generate_class_dependency(owner, static_cast<FStructProperty*>(inner)->GetStruct(), current_class_content);
                    return false;
                }
            }
            else if (property->IsA<FMapProperty>())
            {
                XProperty* key_property = static_cast<FMapProperty*>(property)->GetKeyProp();
                XProperty* value_property = static_cast<FMapProperty*>(property)->GetValueProp();

                if (key_property->IsA<FStructProperty>())
                {
                    generate_class_dependency(owner, static_cast<FStructProperty*>(key_property)->GetStruct(), current_class_content);
                }

                if (value_property->IsA<FStructProperty>())
                {
                    generate_class_dependency(owner, static_cast<FStructProperty*>(value_property)->GetStruct(), current_class_content);
                }

                return false;
            }

            return false;
        }

        auto make_function_info(ObjectInfo& owner, UFunction* function, File::StringType& current_class_content) -> FunctionInfo
        {
            FunctionInfo function_info{
                    .function = function,
                    .owner = owner,
            };

            for (XProperty* param : Unreal::TFieldRange<XProperty>(function, EFieldIterationFlags::IncludeDeprecated))
            {
                if (!param->HasAnyPropertyFlags(Unreal::CPF_Parm | Unreal::CPF_ReturnParm))
                {
                    continue;
                }

                function_info.params.emplace_back(PropertyInfo{param, generate_class_dependency_from_property(owner, param, current_class_content)});
            }

            return function_info;
        }

        auto generate_class(ObjectInfo object_info, GeneratedFile& generated_file, File::StringType& current_class_content) -> XProperty*
        {
            UStruct* native_class = static_cast<UStruct*>(object_info.object);
            if (specification.should_generate_class(native_class))
            {
                generated_file.primary_file_has_no_contents = false;
                specification.generate_class(this, object_info, generated_file, current_class_content);
            }

            return native_class->GetFirstProperty();
        }

        auto generate_enum(UObject* native_object, GeneratedFile& generated_file) -> void
        {
            generated_file.secondary_file_has_no_contents = false;

            File::StringType content_buffer;
            UEnum* uenum = static_cast<UEnum*>(native_object);

#ifdef __linux__
            bool decl_ok = RC::SignalGuard::safe_call([&]() { specification.generate_enum_declaration(content_buffer, uenum); });
            if (!decl_ok) {
                Output::send<LogLevel::Warning>(STR("generate_enum: declaration failed for enum at {:016X}, skipping\n"), (uintptr_t)uenum);
                return;
            }
#else
            specification.generate_enum_declaration(content_buffer, uenum);
#endif

            // Validate and iterate enum names safely
            std::vector<Unreal::FEnumNamePair> safe_elems;
#ifdef __linux__
            bool iter_ok = RC::SignalGuard::safe_call([&]() {
                // Validate TArray before iteration using MemberOffsets if available
                try {
                    // Basic validation of UEnum object
                    if (!uenum) return;
                    // Try to copy elems safely via ForEachName with validation
                    for (const auto& elem : uenum->ForEachName()) {
                        // Validate FName before ToString
                        auto fname_opt = SafeFNameToString(elem.Key, true);
                        if (!fname_opt) continue;
                        safe_elems.push_back(elem);
                        if (safe_elems.size() > 10000) break; // prevent huge enum
                    }
                } catch (...) {}
            });
            if (!iter_ok) {
                Output::send<LogLevel::Warning>(STR("generate_enum: iteration failed for enum at {:016X}\n"), (uintptr_t)uenum);
                return;
            }
#else
            for (const auto& elem : uenum->ForEachName()) safe_elems.push_back(elem);
#endif

            for (const auto& elem : safe_elems)
            {
#ifdef __linux__
                auto fname_opt = SafeFNameToString(elem.Key, true);
                File::StringType enum_value_full_name = fname_opt ? *fname_opt : fmt::format(STR("FName_{:08X}_{}"), elem.Key.GetComparisonIndex().ToUnstableInt(), elem.Key.GetNumber());
#else
                auto enum_value_full_name = elem.Key.ToString();
#endif
                size_t colon_pos = enum_value_full_name.rfind(STR(":"));
                auto enum_value_name = colon_pos == enum_value_full_name.npos ? enum_value_full_name : enum_value_full_name.substr(colon_pos + 1);
                if (enum_value_name.empty() || enum_value_name.size() > 256) {
                    Output::send<LogLevel::Warning>(STR("generate_enum: invalid enum_value_name length {} for enum {:016X}\n"), enum_value_name.size(), (uintptr_t)uenum);
                    continue;
                }
                bool valid = true;
                for (auto c : enum_value_name) {
                    if (!(std::isalnum((unsigned char)c) || c == '_' )) { valid = false; break; }
                }
                if (!valid) {
                    Output::send<LogLevel::Warning>(STR("generate_enum: invalid chars in enum_value_name '{}'\n"), enum_value_name);
                    enum_value_name = fmt::format(STR("VAL_{:08X}"), elem.Key.GetComparisonIndex().ToUnstableInt());
                }
                Output::send<LogLevel::Warning>(STR("generate_enum: member '{}' for enum {:016X} content_buffer {}b\n"), enum_value_name, (uintptr_t)uenum, content_buffer.size());
                std::fprintf(stderr, "generate_enum: member %ls content_buffer %zu\n", enum_value_name.c_str(), content_buffer.size());
#ifdef __linux__
                bool member_ok = RC::SignalGuard::safe_call([&]() { specification.generate_enum_member(content_buffer, uenum, enum_value_name, elem); });
                if (!member_ok) {
                    Output::send<LogLevel::Warning>(STR("generate_enum: member failed for '{}'\n"), enum_value_name);
                    continue;
                }
#else
                specification.generate_enum_member(content_buffer, uenum, enum_value_name, elem);
#endif
                if (content_buffer.size() > 2*1024*1024) {
                    Output::send<LogLevel::Warning>(STR("generate_enum: content_buffer exceeded 2MB ({}b), truncating\n"), content_buffer.size());
                    content_buffer.append(STR("\n// TRUNCATED DUE TO SIZE LIMIT (2MB)\n"));
                    break;
                }
            }

#ifdef __linux__
            bool end_ok = RC::SignalGuard::safe_call([&]() { specification.generate_enum_end(content_buffer, uenum); });
            if (!end_ok) {
                Output::send<LogLevel::Warning>(STR("generate_enum: end failed for enum at {:016X}\n"), (uintptr_t)uenum);
            }
#else
            specification.generate_enum_end(content_buffer, uenum);
#endif

            content_buffer.append(STR("\n\n"));
            if (content_buffer.size() > 2*1024*1024) {
                content_buffer.resize(2*1024*1024);
                content_buffer.append(STR("\n// TRUNCATED DUE TO SIZE LIMIT (2MB)\n"));
            }
            generated_file.ordered_secondary_file_contents.push_back(content_buffer);
        }

        auto generate_package(UObject* package, File::StringType& out) -> void
        {
            UObjectGlobals::ForEachUObject([&](void* object, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index) {
                return LoopAction::Continue;
            });
        }

        auto generate_package_if_non_existent(UObject* object) -> GeneratedFile*
        {
            UObject* package{};
            UObject* Outer = object;
            if (!Outer)
            {
                return nullptr;
            }

            // Diagnostic counter for first few outer chains
            static int diag_outer_count = 0;
#ifdef __linux__
            bool package_ok = RC::SignalGuard::safe_call([&]() {
                // Walk outer chain until UPackage as per spec
                while (Outer && !Outer->IsA<Unreal::UPackage>()) {
                    Outer = Outer->GetOuterPrivate();
                }
                package = Outer;
            });
            if (!package_ok) {
                Output::send<LogLevel::Warning>(STR("generate_package_if_non_existent: outer chain SIGSEGV for object {:016X}\n"), (uintptr_t)object);
                std::fprintf(stderr, "outer chain SIGSEGV for object %p\n", object);
                return nullptr;
            }
            // Diagnostic logging for first few objects
            if (diag_outer_count < 5) {
                bool log_ok = RC::SignalGuard::safe_call([&]() {
                    StringType obj_name = STR("<unknown>");
                    StringType outer_type = STR("<unknown>");
                    StringType outer_name = STR("<unknown>");
                    try {
                        auto opt = SafeFNameToString(object->GetFName(), true);
                        if (opt) obj_name = *opt;
                    } catch (...) {}
                    if (Outer) {
                        try {
                            auto oopt = SafeFNameToString(Outer->GetFName(), true);
                            if (oopt) outer_name = *oopt;
                        } catch (...) {}
                        try {
                            auto cls = Outer->GetClassPrivate();
                            if (cls) {
                                auto copt = SafeFNameToString(cls->GetNamePrivate(), true);
                                if (copt) outer_type = *copt;
                            }
                        } catch (...) {}
                    } else {
                        outer_name = STR("null");
                        outer_type = STR("null");
                    }
                    Output::send<LogLevel::Warning>(STR("generate_package_if_non_existent: diag object '{}' outer type '{}' outer name '{}'\n"), obj_name, outer_type, outer_name);
                    std::fprintf(stderr, "diag object %ls outer type %ls outer name %ls\n", obj_name.c_str(), outer_type.c_str(), outer_name.c_str());
                });
                (void)log_ok;
                diag_outer_count++;
            }
#else
            while (Outer && !Outer->IsA<Unreal::UPackage>()) {
                Outer = Outer->GetOuterPrivate();
            }
            package = Outer;
#endif

            if (!package)
            {
                Output::send<LogLevel::Warning>(STR("generate_package_if_non_existent: no UPackage found for object {:016X}, using Transient\n"), (uintptr_t)object);
                std::fprintf(stderr, "no UPackage for object %p, using Transient\n", object);
                // Use Transient as fallback - create synthetic FName for map key
                // Check if Transient already exists
                std::string transient_key = "T";
                bool found_trans = false;
            for (auto& gf : m_files) if (gf.package_name == File::StringType(transient_key.begin(), transient_key.end())) { found_trans = true; break; }
            if (found_trans) {
                    for (auto& gf : m_files) if (gf.package_name == File::StringType(transient_key.begin(), transient_key.end())) return &gf;
                    return nullptr;
                }
                // For Transient, use package_name "T" directly
                File::StringType package_name = STR("T");
                // Fall through to file creation with transient_key
                // package_fname not needed, use transient_key
                // Need to handle file creation; we will jump to common creation logic
                // To avoid duplicating, we set package_fname and continue to file creation
                // But we need package_name already set, so we handle specially below
                // Instead, we will create file here and return
                {
                    File::StringType pkg_name = STR("T");
                    File::StringType pkg_lower = pkg_name;
                    std::transform(pkg_lower.begin(), pkg_lower.end(), pkg_lower.begin(), [](File::CharType c){ return std::towlower(c); });
                    if (m_file_names.contains(pkg_lower)) {
                        auto& fn = m_file_names[pkg_lower];
                        pkg_name.append(fmt::format(STR("_DUPL_{}"), ++fn.num_collisions));
                    } else {
                        m_file_names.emplace(pkg_lower, FileName{});
                    }
                    std::string dir_narrow = m_dir_narrow;
                    if (dir_narrow.empty() || dir_narrow.size() > 256) dir_narrow = "CXXHeaderDump";
                    File::StringType ext_u16 = specification.get_file_extension();
                    auto to_narrow = [](const File::StringType& s) -> std::string {
                        std::string r; r.reserve(s.size() > 1000 ? 1000 : s.size());
                        for (size_t i=0;i<s.size() && i<1000;i++) r.push_back(static_cast<char>(s[i] & 0xFF));
                        return r;
                    };
                    std::string ext = to_narrow(ext_u16);
                    if (ext.empty() || ext[0] != '.') ext = "." + ext;
                    std::string pkg_narrow = to_narrow(pkg_name);
                    if (pkg_narrow.empty() || pkg_narrow.size() > 128) pkg_narrow = "F";
                        // p1/p2 not needed, will compute later from package_name
                    GeneratedFile gf{ .ordered_primary_file_contents = {}, .ordered_secondary_file_contents = {}, .package_name = pkg_name, .primary_file = {}, .secondary_file = {}, .primary_file_has_no_contents = true, .secondary_file_has_no_contents = true };
                    GeneratedFile* res = nullptr;
#ifdef __linux__
                    bool ok = RC::SignalGuard::safe_call([&](){ m_files.push_back(std::move(gf)); auto& f = m_files.back(); res = &f; });
                    if (!ok || !res) return nullptr;
#else
                    m_files.push_back(std::move(gf)); auto& f = m_files.back(); res = &f;
#endif
                    return res;
                }
            }

            // Derive package_name first via SafeFName before checking map (convert to narrow string for map key to avoid u16 heap)
            std::string package_name_pre;
            {
                auto pkg_opt_pre = SafeFNameToString(package->GetFName(), true);
                File::StringType tmp_w;
                if (!pkg_opt_pre || pkg_opt_pre->empty() || pkg_opt_pre->size() > 256) {
                    pkg_opt_pre = SafeFNameToString(package->GetNamePrivate(), true);
                }
                if (pkg_opt_pre && !pkg_opt_pre->empty() && pkg_opt_pre->size() <= 256) {
                    tmp_w = *pkg_opt_pre;
                    size_t slash = tmp_w.rfind(STR("/"));
                    if (slash != tmp_w.npos) tmp_w = tmp_w.substr(slash + 1);
                }
                if (tmp_w.empty() || tmp_w.size() > 128) {
                    tmp_w = fmt::format(STR("P_{:04X}"), package->GetFName().GetComparisonIndex().ToUnstableInt() & 0xFFFF);
                }
                // Convert from File::StringType (char16) to std::string (char)
                package_name_pre.clear();
                package_name_pre.reserve(tmp_w.size());
                for (auto c : tmp_w) package_name_pre.push_back(static_cast<char>(c & 0xFF));
                if (package_name_pre.empty() || package_name_pre.size() > 128) {
                    package_name_pre = fmt::format("P_{:04X}", package->GetFName().GetComparisonIndex().ToUnstableInt() & 0xFFFF);
                }
            }
            std::string map_key = package_name_pre;
            // Validate map_key chars
            {
                bool valid=true; for(auto c: map_key) if(!(std::isalnum((unsigned char)c)||c=='_'||c=='-')){valid=false;break;}
                if(!valid) map_key = fmt::format("P_{:04X}", package->GetFName().GetComparisonIndex().ToUnstableInt() & 0xFFFF);
            }
            bool found_existing = false;
            GeneratedFile* existing_ptr = nullptr;
            for (auto& gf : m_files) if (gf.package_name == File::StringType(map_key.begin(), map_key.end())) { found_existing = true; existing_ptr = &gf; break; }
            if (found_existing)
            {
                for (auto& gf : m_files) if (gf.package_name == File::StringType(map_key.begin(), map_key.end())) return &gf;
                return nullptr; // should not happen
            }
            else
            {
                // Get rid of everything before the last slash + the last slash, leaving only the actual name
                File::StringType package_name;
                // Convert map_key (narrow) to File::StringType (wide) for package_name
                {
                    File::StringType tmp_w(map_key.begin(), map_key.end());
                    package_name = tmp_w;
                }
#ifdef __linux__
                auto pkg_opt = SafeFNameToString(package->GetNamePrivate(), true);
                if (!pkg_opt || pkg_opt->empty() || pkg_opt->size() > 256) {
                    Output::send<LogLevel::Warning>(STR("generate_package_if_non_existent: invalid package FName idx {:08X}, using synthetic\n"), package->GetNamePrivate().GetComparisonIndex().ToUnstableInt());
                    std::fprintf(stderr, "generate_package invalid FName %08X\n", package->GetNamePrivate().GetComparisonIndex().ToUnstableInt());
                    package_name = fmt::format(STR("P_{:04X}"), package->GetNamePrivate().GetComparisonIndex().ToUnstableInt() & 0xFFFF);
                } else {
                    package_name = *pkg_opt;
                }
#else
                package_name = package->GetNamePrivate().ToString();
#endif
                // Validate package_name length and chars before substr
                if (package_name.size() > 512) package_name = package_name.substr(0, 512);
                {
                    size_t slash = package_name.rfind(STR("/"));
                    if (slash != package_name.npos) package_name = package_name.substr(slash + 1);
                }
                if (package_name.empty() || package_name.size() > 128) {
                    Output::send<LogLevel::Warning>(STR("generate_package_if_non_existent: invalid package_name '{}' length {} -> synthetic\n"), package_name.substr(0,32), package_name.size());
                    package_name = fmt::format(STR("P_{:04X}"), package->GetNamePrivate().GetComparisonIndex().ToUnstableInt() & 0xFFFF);
                }
                {
                    bool valid = true;
                    for (auto c : package_name) {
                        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-' )) { valid = false; break; }
                    }
                    if (!valid) {
                        Output::send<LogLevel::Warning>(STR("generate_package_if_non_existent: invalid chars in package_name '{}' -> synthetic\n"), package_name);
                        package_name = fmt::format(STR("P_{:04X}"), package->GetNamePrivate().GetComparisonIndex().ToUnstableInt() & 0xFFFF);
                    }
                }
                File::StringType package_name_all_lower = package_name;
                std::transform(package_name_all_lower.begin(), package_name_all_lower.end(), package_name_all_lower.begin(), [](File::CharType c) {
                    return std::towlower(c);
                });

                if (m_file_names.contains(package_name_all_lower))
                {
                    // File name collision
                    auto& file_name = m_file_names[package_name_all_lower];
                    package_name.append(fmt::format(STR("_DUPL_{}"), ++file_name.num_collisions));
                    Output::send(STR("File name collision, renamed to '{}'\n"), package_name);
                }
                else
                {
                    m_file_names.emplace(package_name_all_lower, FileName{});
                }

                std::string dir_narrow2 = m_dir_narrow;
                if (dir_narrow2.empty() || dir_narrow2.size() > 256) dir_narrow2 = "CXXHeaderDump";
                File::StringType ext_u16 = specification.get_file_extension();
                auto to_narrow2 = [](const File::StringType& s) -> std::string {
                    std::string r; r.reserve(s.size() > 1000 ? 1000 : s.size());
                    for (size_t i=0;i<s.size() && i<1000;i++) r.push_back(static_cast<char>(s[i] & 0xFF));
                    return r;
                };
                std::string ext = to_narrow2(ext_u16);
                if (ext.empty() || ext[0] != '.') ext = "." + ext;
                std::string pkg_narrow2 = to_narrow2(package_name);
                if (pkg_narrow2.empty() || pkg_narrow2.size() > 128) pkg_narrow2 = "F";
                GeneratedFile generated_file{
                        .ordered_primary_file_contents = {},
                        .ordered_secondary_file_contents = {},
                        .package_name = package_name,
                        .primary_file = {},
                        .secondary_file = {},
                        .primary_file_has_no_contents = true,
                        .secondary_file_has_no_contents = true,
                };

                // Diagnostic: log intended package_name before emplace
                Output::send<LogLevel::Warning>(STR("generate_package_if_non_existent: emplacing package_name '{}' size {} for key '{}'\n"), package_name.substr(0,32), package_name.size(), File::StringType(map_key.begin(), map_key.end()).substr(0,32));
                std::fprintf(stderr, "emplacing package %ls size %zu key %s\n", package_name.substr(0,32).c_str(), package_name.size(), map_key.substr(0,32).c_str());
                // Heap canary check before emplace
                void* canary_before = malloc(64);
                if (canary_before) { memset(canary_before, 0xAA, 64); }
                GeneratedFile* result_ptr = nullptr;
#ifdef __linux__
                bool emplace_ok = RC::SignalGuard::safe_call([&]() {
                    m_files.push_back(std::move(generated_file));
                    result_ptr = &m_files.back();
                    // Set package_name for map_key is already in generated_file.package_name, no need for key
                });
                // Check canary after emplace
                if (canary_before) {
                    bool corrupted = false;
                    for (int i=0;i<64;i++) if (((unsigned char*)canary_before)[i] != 0xAA) { corrupted=true; break; }
                    if (corrupted) {
                        Output::send<LogLevel::Warning>(STR("HEAP CORRUPTION detected after emplace for package '{}'\n"), package_name.substr(0,32));
                        std::fprintf(stderr, "HEAP CORRUPTION after emplace %ls\n", package_name.substr(0,32).c_str());
                    }
                    // Also check result_ptr's package_name size
                    if (result_ptr) {
                        size_t sz = 0;
                        bool sz_ok = RC::SignalGuard::safe_call([&](){ sz = result_ptr->package_name.size(); });
                        if (!sz_ok || sz > 128) {
                            Output::send<LogLevel::Warning>(STR("post-emplace size check failed for '{}' got {}\n"), package_name.substr(0,32), sz);
                        }
                    }
                    free(canary_before);
                }
                if (!emplace_ok || !result_ptr) {
                    Output::send<LogLevel::Warning>(STR("generate_package_if_non_existent: emplace failed for package\n"));
                    std::fprintf(stderr, "emplace failed\n");
                    return nullptr;
                }
                // Validate after emplace package_name not corrupted
                if (result_ptr->package_name.empty() || result_ptr->package_name.size() > 128) {
                    Output::send<LogLevel::Warning>(STR("generate_package_if_non_existent: post-emplace invalid package_name length {}\n"), result_ptr->package_name.size());
                    result_ptr->package_name = STR("F");
                }
                return result_ptr;
#else
                m_files.push_back(std::move(generated_file));
                auto& file_in_map = m_files.back();
                return &file_in_map;
#endif
            }
        }

        auto cleanup_old_sdk() -> void
        {
            if (!std::filesystem::exists(m_directory_to_generate_in))
            {
                return;
            }

            for (const auto& item : std::filesystem::directory_iterator(m_directory_to_generate_in))
            {
                if (item.is_directory())
                {
                    continue;
                }
                if (item.path().extension() != specification.get_file_extension())
                {
                    continue;
                }

                File::delete_file(item.path());
            }
        }

      public:
        auto generate() -> void
        {
#ifdef __linux__
            bool is_child_process = false;
            pid_t pid = fork();
            if (pid == -1) {
                Output::send<LogLevel::Warning>(STR("CXX fork failed, falling back to direct generation\n"));
                // fall through to direct generation
            } else if (pid > 0) {
                // Parent: wait for child
                int status = 0;
                pid_t w = waitpid(pid, &status, 0);
                if (w == -1) {
                    Output::send<LogLevel::Warning>(STR("waitpid failed\n"));
                } else {
                    if (WIFEXITED(status)) {
                        int code = WEXITSTATUS(status);
                        Output::send(STR("CXX generation child exited with code {}\n"), code);
                        if (code == 0) {
                            Output::send(STR("CXX generation completed successfully in child\n"));
                        } else {
                            Output::send<LogLevel::Warning>(STR("CXX generation child failed with code {}, partial output may exist\n"), code);
                        }
                    } else if (WIFSIGNALED(status)) {
                        Output::send<LogLevel::Warning>(STR("CXX generation child terminated by signal {}\n"), WTERMSIG(status));
                    }
                }
                // Parent done, files should be on disk from child
                return;
            } else {
                // Child: continue to do generation, will _exit at end
                is_child_process = true;
                Output::send(STR("CXX generation in child process {}\n"), getpid());
            }
#endif
            Output::send(STR("Cleaning up old SDK files...\n"));
            cleanup_old_sdk();
            Output::send(STR("Generating SDK...\n"));

            // 400k should be enough for most games, and it's highly unlikely to cause more than one reallocation even if the game is huge
            m_classes_dumped.reserve(400000);
            m_files.reserve(10000);

            size_t num_objects_generated{};
            size_t skipped_pendingkill = 0, failed_enum = 0, failed_class = 0;
            UObjectGlobals::ForEachUObject([&](void* untyped_object, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index) {
                std::fprintf(stderr, "ForEachUObject: entered chunk %d index %d object %p\n", chunk_index, object_index, untyped_object);
                std::fflush(stderr);
                UObject* object = static_cast<UObject*>(untyped_object);
                // Validate object pointer before use
                if (!untyped_object) return LoopAction::Continue;
#ifdef __linux__
                // Fast pending kill check: only check FName block, avoid heavy IsUnreachable SignalGuard for performance
                // IsUnreachable is expensive and triggers many signals, so we only do lightweight checks
                bool is_pending = false;
                // Quick FName block check without SignalGuard (just integer ops)
                {
                    uint32_t idx = 0;
                    bool idx_ok = RC::SignalGuard::safe_call([&]() { idx = object->GetFName().GetComparisonIndex().ToUnstableInt(); });
                    if (!idx_ok) { ++skipped_pendingkill; return LoopAction::Continue; }
                    uint32_t block = idx >> 16;
                    if (block >= 8192) { ++skipped_pendingkill; return LoopAction::Continue; }
                    // Also check Number range
                    int32_t num = 0;
                    bool num_ok = RC::SignalGuard::safe_call([&]() { num = object->GetFName().GetNumber(); });
                    if (!num_ok || num < -1 || num > 100000) { ++skipped_pendingkill; return LoopAction::Continue; }
                }
                // For performance, skip IsUnreachable check and outer chain check for now
                // These are heavy and cause many signals; lightweight FName check is sufficient to filter most corrupted objects
                // is_pending remains false
#endif
                // Wrap GetClassPrivate in SignalGuard to handle corrupted objects
                UClass* object_class = nullptr;
#ifdef __linux__
                bool class_ok = RC::SignalGuard::safe_call([&]() { object_class = object->GetClassPrivate(); });
                if (!class_ok || !object_class) { ++failed_class; return LoopAction::Continue; }
                // Validate class FName as well
                {
                    bool class_valid = false;
                    RC::SignalGuard::safe_call([&]() {
                        auto cfname = object_class->GetFName();
                        uint32_t cidx = cfname.GetComparisonIndex().ToUnstableInt();
                        uint32_t cblock = cidx >> 16;
                        class_valid = (cblock < 8192);
                    });
                    if (!class_valid) { ++failed_class; return LoopAction::Continue; }
                }
#else
                UClass* object_class = nullptr;
                try { object_class = object->GetClassPrivate(); } catch (...) { return LoopAction::Continue; }
                if (!object_class) return LoopAction::Continue;
#endif

                // Generate file for package if it doesn't already exist
                GeneratedFile* package_file = generate_package_if_non_existent(object);

                if (!package_file)
                {
                    // Object should not be dumped
                    return LoopAction::Continue;
                }
                // Wrap IsA checks in SignalGuard to handle corrupted objects
                bool is_enum = false;
                bool is_class_candidate = false;
#ifdef __linux__
                bool is_check_ok = RC::SignalGuard::safe_call([&]() {
                    is_enum = object->IsA<UEnum>();
                    if (!is_enum) {
                        is_class_candidate = (object_class->IsChildOf<UClass>() || object_class->IsChildOf<UScriptStruct>()) && !m_classes_dumped.contains(object);
                    }
                });
                if (!is_check_ok) { ++failed_class; return LoopAction::Continue; }
                if (is_enum)
#else
                if (object->IsA<UEnum>())
#endif
                {
                    generate_enum(object, *package_file);
                    ++num_objects_generated;

                    return LoopAction::Continue;
                }
#ifdef __linux__
                else if (is_class_candidate)
#else
                else if ((object_class->IsChildOf<UClass>() || object_class->IsChildOf<UScriptStruct>()) && !m_classes_dumped.contains(object))
#endif
                {
                    // Generate a class for this object
                    auto& object_info = m_classes_dumped.emplace(object, ObjectInfo{object}).first->second;
                    File::StringType class_content{};
                    generate_class(object_info, *package_file, class_content);
                    if (!package_file->primary_file_has_no_contents)
                    {
                        package_file->ordered_primary_file_contents.push_back(class_content);
                    }
                    ++num_objects_generated;

                    return LoopAction::Continue;
                }
                else
                {
                    // Object should not be dumped
                    return LoopAction::Continue;
                }
            });

            create_all_files();
#ifdef __linux__
            if (is_child_process) {
                Output::send(STR("CXX child generation finished, exiting\n"));
                // Ensure files are flushed
                std::fflush(nullptr);
                _exit(0);
            }
#endif
        }
    };

    class CXXHeaderGenerator
    {
      public:
        auto get_file_extension() -> File::StringType
        {
            return STR(".hpp");
        }
        auto generate_file_header(GeneratedFile& generated_file) -> void
        {
            generated_file.primary_file.write_string_to_file(
                    fmt::format(STR("#ifndef UE4SS_SDK_{}_HPP\n#define UE4SS_SDK_{}_HPP\n\n"), generated_file.package_name, generated_file.package_name));

            if (!generated_file.secondary_file_has_no_contents)
            {
                std::string pkg_narrow2;
                pkg_narrow2.reserve(generated_file.package_name.size());
                for (auto c : generated_file.package_name) pkg_narrow2.push_back(static_cast<char>(c & 0xFF));
                std::string sec_fname = pkg_narrow2 + "_enums.hpp";
                File::StringType sec_w(sec_fname.begin(), sec_fname.end());
                generated_file.primary_file.write_string_to_file(
                        fmt::format(STR("#include \"{}\"\n\n"), sec_w));
            }
        }
        auto generate_file_footer(GeneratedFile& generated_file) -> void
        {
            generated_file.primary_file.write_string_to_file(fmt::format(STR("#endif\n")));
        }
        auto generate_enum_declaration(File::StringType& content_buffer, UEnum* uenum) -> void
        {
            const auto cpp_form = uenum->GetCppForm();
            File::StringType ename;
            bool ename_ok = RC::SignalGuard::safe_call([&](){ ename = get_native_enum_name(uenum, false); });
            if (!ename_ok || ename.empty()) ename = STR("EnumFallback");
            Output::send<RC::LogLevel::Warning>(STR("generate_enum_declaration: cpp_form {} for enum '{}'\n"), (int)cpp_form, ename);
            std::fprintf(stderr, "generate_enum_declaration: cpp_form %d for enum %ls\n", (int)cpp_form, ename.c_str());
            if (cpp_form == UEnum::ECppForm::Regular)
            {
                content_buffer.append(fmt::format(STR("enum {} {{\n"), ename));
            }
            else if (cpp_form == UEnum::ECppForm::Namespaced)
            {
                content_buffer.append(fmt::format(STR("namespace {} {{\n{}enum Type {{\n"), ename, generate_tab()));
            }
            else if (cpp_form == UEnum::ECppForm::EnumClass)
            {
                content_buffer.append(fmt::format(STR("enum class {} {{\n"), ename));
            }
            else
            {
                Output::send<RC::LogLevel::Warning>(STR("generate_enum_declaration: unknown cpp_form {} for enum '{}', using Regular fallback\n"), (int)cpp_form, ename);
                content_buffer.append(fmt::format(STR("enum {} {{\n"), ename));
            }
        }
        auto generate_enum_member(File::StringType& content_buffer, UEnum* uenum, const File::StringType& enum_value_name, const Unreal::FEnumNamePair& elem) -> void
        {
            content_buffer.append(fmt::format(STR("{}{}{} = {},\n"),
                                              generate_tab(),
                                              uenum->GetCppForm() == UEnum::ECppForm::Namespaced ? generate_tab() : STR(""),
                                              enum_value_name,
                                              elem.Value));
        }
        auto generate_enum_end(File::StringType& content_buffer, UEnum* uenum) -> void
        {
            const auto cpp_form = uenum->GetCppForm();
            content_buffer.append(fmt::format(STR("{}}};"), cpp_form == UEnum::ECppForm::Namespaced ? generate_tab() : STR("")));

            if (cpp_form == UEnum::ECppForm::Namespaced)
            {
                content_buffer.append(STR("\n}"));
            }
        }
        auto should_generate_class(UStruct* native_class)
        {
            return true;
        }
        auto generate_class(TypeGenerator<CXXHeaderGenerator>* generator, ObjectInfo& object_info, GeneratedFile& generated_file, File::StringType& current_class_content)
        {
            UStruct* native_class = static_cast<UStruct*>(object_info.object);
            File::StringType content_buffer{};

            UStruct* inherits_from_class = native_class->GetSuperStruct();

            // Make sure that the base class is defined
            generator->generate_class_dependency(object_info, inherits_from_class, current_class_content);
            Output::send<LogLevel::Warning>(STR("generate_class: '{}' super '{}' for package '{}' content_buffer {}b\n"), native_class->GetName(), inherits_from_class ? inherits_from_class->GetName() : STR("None"), generated_file.package_name, content_buffer.size());

            // If any properties have dependencies, make sure that they are defined
            // This makes sure that we don't have member variables with undefined types (if the types are local, otherwise we need to include the file that the struct exists in)
            std::vector<PropertyInfo> properties_to_generate{};
            for (XProperty* property : Unreal::TFieldRange<XProperty>(native_class, EFieldIterationFlags::IncludeDeprecated))
            {
                properties_to_generate.emplace_back(
                        PropertyInfo{property, generator->generate_class_dependency_from_property(object_info, property, current_class_content)});
            }

            std::vector<FunctionInfo> functions_to_generate{};
            for (UFunction* function : Unreal::TFieldRange<UFunction>(native_class, EFieldIterationFlags::None))
            {
                auto& function_info = functions_to_generate.emplace_back(FunctionInfo{function, object_info});

                for (XProperty* param : Unreal::TFieldRange<XProperty>(function, EFieldIterationFlags::IncludeDeprecated))
                {
                    if (!param->HasAnyPropertyFlags(Unreal::CPF_Parm | Unreal::CPF_ReturnParm))
                    {
                        continue;
                    }

                    function_info.params.emplace_back(
                            PropertyInfo{param, generator->generate_class_dependency_from_property(object_info, param, current_class_content)});
                }
            }

            auto class_name = generate_class_name(native_class);
            Output::send<LogLevel::Warning>(STR("generate_class: class_name '{}' for package '{}'\n"), class_name, generated_file.package_name);

            generate_class_declaration(content_buffer, native_class, inherits_from_class);

            int32_t num_padding_elements{0};
            XProperty* last_property_in_this_class{nullptr};

            for (const auto& property_info : properties_to_generate)
            {
#ifdef __linux__
                bool prop_ok = false;
                prop_ok = RC::SignalGuard::safe_call([&]() {
#endif
                    XProperty* property = property_info.property;
                    if (!property) return;
                    int32_t current_property_offset = 0, current_property_size = 0;
                    try {
                        current_property_offset = property->GetOffset_Internal();
                        current_property_size = property->GetSize();
                        if (current_property_offset < 0 || current_property_offset > 0x20000) return;
                        if (current_property_size < 0 || current_property_size > 0x10000) return;
                        if (current_property_offset + current_property_size > 0x30000) return;
                    } catch (...) { return; }
                    StringType prop_name;
#ifdef __linux__
                    try {
                        auto opt = SafeFNameToString(property->GetFName(), true);
                        prop_name = opt ? *opt : STR("<invalid>");
                        if (prop_name.size() > 256 || prop_name.empty()) prop_name = fmt::format(STR("Prop_{:08X}"), property->GetFName().GetComparisonIndex().ToUnstableInt());
                    } catch (...) { prop_name = STR("<name_failed>"); }
#else
                    try { prop_name = property->GetName(); } catch (...) { prop_name = STR("<name_failed>"); }
#endif
                    Output::send<LogLevel::Warning>(STR("  generate_class: property '{}' for class '{}' content_buffer {}b\n"), prop_name, class_name, content_buffer.size());

                StringType part_one{};
                try
                {
                    part_one = fmt::format(STR("{}{}{} {};"),
                                           generate_tab(),
                                           property_info.should_forward_declare ? STR("class ") : STR(""),
                                           generate_property_cxx_name(property, true, native_class, EnableForwardDeclarations::Yes),
#ifdef __linux__
                                           ([&]() -> StringType {
                                               try {
                                                   auto opt = SafeFNameToString(property->GetFName(), true);
                                                   auto s = opt ? *opt : fmt::format(STR("Prop_{:08X}"), property->GetFName().GetComparisonIndex().ToUnstableInt());
                                                   if (s.empty() || s.size()>256) s = fmt::format(STR("Prop_{:08X}"), property->GetFName().GetComparisonIndex().ToUnstableInt());
                                                   return s;
                                               } catch (...) { return STR("Prop_Failed"); }
                                           }())
#else
                                           property->GetName()
#endif
                                           );
                }
                catch (std::exception& e)
                {
                    Output::send<LogLevel::Warning>(STR("Could not generate property '{}' because: {}\n"), property->GetFullName(), ensure_str(e.what()));
                    return;
                } catch (...) { return; }

                content_buffer.append(fmt::format(STR("{}\n"), generator->generate_offset_comment(property, part_one)));

                if (property->IsA<FDelegateProperty>())
                {
                    generator->generate_function_declaration(object_info,
                                                             generator->make_function_info(object_info,
                                                                                           static_cast<FDelegateProperty*>(property)->GetSignatureFunction(),
                                                                                           current_class_content),
                                                             generated_file,
                                                             content_buffer,
                                                             IsDelegateFunction::Yes);
                }
                else if (property->IsA<FMulticastInlineDelegateProperty>())
                {
                    generator->generate_function_declaration(
                            object_info,
                            generator->make_function_info(object_info,
                                                          static_cast<FMulticastInlineDelegateProperty*>(property)->GetSignatureFunction(),
                                                          current_class_content),
                            generated_file,
                            content_buffer,
                            IsDelegateFunction::Yes);
                }
                else if (property->IsA<FMulticastSparseDelegateProperty>())
                {
                    generator->generate_function_declaration(
                            object_info,
                            generator->make_function_info(object_info,
                                                          static_cast<FMulticastSparseDelegateProperty*>(property)->GetSignatureFunction(),
                                                          current_class_content),
                            generated_file,
                            content_buffer,
                            IsDelegateFunction::Yes);
                }

                if (UE4SSProgram::settings_manager.CXXHeaderGenerator.KeepMemoryLayout)
                {
                    // Check if next member-var is reflected
                    // If it's not, add padding so that everything in the struct is aligned properly
                    auto* next_property = property->GetNextFieldAsProperty();
                    if (next_property)
                    {
                        int32_t current_property_end_location = current_property_offset + current_property_size;

                        int32_t next_property_offset = next_property->GetOffset_Internal();

                        if (current_property_offset != next_property_offset && current_property_end_location != next_property_offset)
                        {
                            // Add padding
                            int32_t padding_property_offset = current_property_end_location;
                            int32_t padding_property_size = next_property_offset - padding_property_offset;

                            auto padding_part_one = fmt::format(STR("{}char {}[0x{:X}];"),
                                                                generate_tab(),
                                                                fmt::format(STR("padding_{}"), num_padding_elements++),
                                                                padding_property_size);
                            content_buffer.append(
                                    fmt::format(STR("{:85} // 0x{:04X} (size: 0x{:X})\n"), padding_part_one, padding_property_offset, padding_property_size));
                        }
                    }
                }

                last_property_in_this_class = property;
                    // Final hard limit check after property fully processed
                    if (content_buffer.size() > 2*1024*1024) {
                        Output::send<LogLevel::Warning>(STR("generate_class: content_buffer exceeded 2MB after property '{}' for class '{}' ({}b), truncating\n"), prop_name, class_name, content_buffer.size());
                        content_buffer.append(STR("\n// TRUNCATED DUE TO SIZE LIMIT (2MB) - remaining properties skipped\n"));
                    }
#ifdef __linux__
                });
                if (!prop_ok) {
                    StringType prop_name2;
#ifdef __linux__
                    try {
                        if (property_info.property) {
                            auto opt = SafeFNameToString(property_info.property->GetFName(), true);
                            prop_name2 = opt ? *opt : STR("<invalid>");
                            if (prop_name2.size() > 256 || prop_name2.empty()) prop_name2 = fmt::format(STR("Prop_{:08X}"), property_info.property->GetFName().GetComparisonIndex().ToUnstableInt());
                        } else prop_name2 = STR("<null>");
                    } catch (...) { prop_name2 = STR("<name_failed>"); }
#else
                    try { prop_name2 = property_info.property ? property_info.property->GetName() : STR("<null>"); } catch (...) { prop_name2 = STR("<name_failed>"); }
#endif
                    Output::send<LogLevel::Warning>(STR("generate_class: SignalGuard caught SIGSEGV for property '{}' at {:016X} in class '{}', skipping\n"), prop_name2, (uintptr_t)property_info.property, class_name);
                    continue;
                }
                if (content_buffer.size() > 2*1024*1024) {
                    Output::send<LogLevel::Warning>(STR("generate_class: breaking loop for class '{}' due to 2MB limit ({}b)\n"), class_name, content_buffer.size());
                    break;
                }
#endif
            }

            // Hard limit: truncate if still exceeds 2MB after all processing
            if (content_buffer.size() > 2*1024*1024) {
                Output::send<LogLevel::Warning>(STR("generate_class: content_buffer exceeded 2MB for class '{}' final size {}b, truncating\n"), class_name, content_buffer.size());
                std::fprintf(stderr, "generate_class: final content_buffer exceeded 2MB (%zu)\n", content_buffer.size());
                if (content_buffer.size() > 2*1024*1024 + 100) {
                    content_buffer.resize(2*1024*1024);
                    content_buffer.append(STR("\n// TRUNCATED DUE TO SIZE LIMIT (2MB)\n"));
                }
            }

            int32_t class_size = native_class->GetPropertiesSize();
            generate_class_struct_end(content_buffer, class_name, class_size, num_padding_elements, last_property_in_this_class);

            // Functions
            if (native_class->HasChildren())
            {
                content_buffer.append(STR("\n"));
                for (const auto& function_info : functions_to_generate)
                {
                    generator->generate_function_declaration(object_info, function_info, generated_file, content_buffer);
                }
            }

            generate_class_end(content_buffer, class_size);

            content_buffer.append(STR("\n\n"));

            current_class_content.append(content_buffer);
        }

        auto generate_class_declaration(File::StringType& content_buffer, UStruct* native_class, UStruct* inherits_from_class) -> void
        {
            auto class_name = generate_class_name(native_class);
            if (inherits_from_class)
            {
                content_buffer.append(
                        fmt::format(STR("{} {} : public {}\n{{\n"), generate_prefix(native_class), class_name, generate_class_name(inherits_from_class)));
            }
            else
            {
                content_buffer.append(fmt::format(STR("{} {}\n{{\n"), generate_prefix(native_class), class_name));
            }
        }
        auto generate_class_struct_end(File::StringType& content_buffer,
                                       const File::StringType& class_name,
                                       size_t class_size,
                                       int32_t num_padding_elements,
                                       XProperty* last_property_in_this_class) -> void
        {
            if (UE4SSProgram::settings_manager.CXXHeaderGenerator.KeepMemoryLayout)
            {
                if (last_property_in_this_class)
                {
                    // TODO: Fix this, the padding is required for alignment when using the SDK for code injection
                    // This commented-out code was here to provide the correct padding between classes so that everything would line up correctly
                    // But it no longer works because it was dependent on the "generate_class_chain" function that no longer eixsts
                    /*
                    int32_t last_property_offset = last_property_in_this_class->get_offset_for_internal();
                    if (first_property)
                    {
                        int32_t first_property_offset = first_property->get_offset_for_internal();
                        if (last_property_offset != first_property_offset)
                        {
                            int32_t last_property_size = last_property_in_this_class->get_size();
                            int32_t padding_size = first_property_offset - (last_property_offset + last_property_size);
                            printf_s("class_size: %X\n", class_size);
                            printf_s("first_property->get_size(): %X\n", first_property->get_size());
                            printf_s("last_property_offset: %X\n", last_property_offset);
                            printf_s("first_property_offset: %X\n", first_property_offset);

                            auto padding_part_one = fmt::format(STR("{}char {}[0x{:X}];"), generate_tab(), fmt::format(STR("padding_{}"), num_padding_elements), padding_size);
                            out.append(fmt::format(STR("{:85} // 0x{:04X} (size: 0x{:X})\n"), padding_part_one, last_property_offset + last_property_size, padding_size));
                        }
                    }
                    //*/
                }
                else if (class_size > 0)
                {
                    // No reflected member variables exist but there are non-reflected member variables
                    // Add padding for non-reflected member variables, for alignment purposes
                    auto padding_part_one =
                            fmt::format(STR("{}char {}[0x{:X}];"), generate_tab(), fmt::format(STR("padding_{}"), num_padding_elements), class_size);
                    content_buffer.append(fmt::format(STR("{:85} // 0x0000 (size: 0x{:X})\n"), padding_part_one, 0x0));
                }
            }
        }
        auto generate_class_end(File::StringType& content_buffer, size_t class_size) -> void
        {
            if (UE4SSProgram::settings_manager.CXXHeaderGenerator.DumpOffsetsAndSizes)
            {
                content_buffer.append(fmt::format(STR("}}; // Size: 0x{:X}"), class_size));
            }
            else
            {
                content_buffer.append(STR("};"));
            }
        }

        auto generate_function_declaration(TypeGenerator<CXXHeaderGenerator>* generator,
                                           File::StringType& current_class_content,
                                           ObjectInfo& owner,
                                           const FunctionInfo& function_info,
                                           File::StringType function_name,
                                           XProperty* return_property,
                                           std::optional<PropertyInfo> return_property_info) -> void
        {
            File::StringType function_type_name{};
            if (return_property)
            {
                try
                {
                    function_type_name = generate_property_cxx_name(return_property, true, function_info.function, EnableForwardDeclarations::Yes);
                }
                catch (std::exception& e)
                {
                    Output::send<LogLevel::Warning>(STR("Could not generate function '{}' because: {}\n"),
                                                    function_info.function->GetFullName(),
                                                    ensure_str(e.what()));
                    return;
                }

                if (return_property_info.value().should_forward_declare && !generator->check_ignore_forward_declaration(owner, return_property))
                {
                    function_type_name.insert(0, STR("class "));
                }
            }
            else
            {
                function_type_name = STR("void");
            }

            current_class_content.append(fmt::format(STR("{}{} {}("), generate_tab(), function_type_name, function_name));

            for (size_t i = 0; i < function_info.params.size(); ++i)
            {
                const auto& param_info = function_info.params[i];
                if (!param_info.property->HasAnyPropertyFlags(Unreal::CPF_ReturnParm))
                {
                    try
                    {
                        current_class_content.append(
                                fmt::format(STR("{}{}{}{} {}"),
                                            param_info.property->HasAnyPropertyFlags(Unreal::CPF_ConstParm) ? STR("const ") : STR(""),
                                            param_info.should_forward_declare ? STR("class ") : STR(""),
                                            generate_property_cxx_name(param_info.property, true, function_info.function, EnableForwardDeclarations::Yes),
                                            param_info.property->HasAnyPropertyFlags(Unreal::CPF_ReferenceParm | Unreal::CPF_OutParm) ? STR("&") : STR(""),
                                            param_info.property->GetName()));
                    }
                    catch (std::exception& e)
                    {
                        Output::send<LogLevel::Warning>(STR("Could not generate function '{}' because: {}\n"),
                                                        function_info.function->GetFullName(),
                                                        ensure_str(e.what()));
                        return;
                    }

                    if (i + 1 < function_info.params.size())
                    {
                        auto* next_param = function_info.params[i + 1].property;
                        if (next_param && (!next_param->HasAnyPropertyFlags(Unreal::CPF_ReturnParm) || i + 2 < function_info.params.size()))
                        {
                            current_class_content.append(STR(", "));
                        }
                    }
                }
            }
            current_class_content.append(STR(");"));
        }
    };

    class LuaTypesGenerator
    {
      private:
        auto is_valid_lua_symbol(const File::StringType& str) -> bool
        {
            static const std::set<File::StringType> keywords = {STR("and"),   STR("break"), STR("do"),       STR("else"),   STR("elseif"), STR("end"),
                                                                STR("false"), STR("for"),   STR("function"), STR("if"),     STR("in"),     STR("local"),
                                                                STR("nil"),   STR("not"),   STR("or"),       STR("repeat"), STR("return"), STR("then"),
                                                                STR("true"),  STR("until"), STR("while")};
            if (keywords.contains(str))
            {
                return false;
            }
            auto it = str.begin();
            if (it == str.end() || std::isdigit(*it))
            {
                // string empty or first char is digit
                return false;
            }
            for (; it != str.end(); ++it)
            {
                auto c = *it;
                if (c != '_' && !std::isalnum(c))
                {
                    // is not underscore or alphanumeric in current locale
                    return false;
                }
            }
            return true;
        }
        auto quote_lua_symbol(const File::StringType& symbol) -> File::StringType
        {
            File::StringType quoted;
            quoted.reserve(symbol.size() + 2);
            quoted.push_back('\'');
            for (auto it = symbol.begin(); it != symbol.end(); ++it)
            {
                auto c = *it;
                if (c == '\\' || c == '\'')
                {
                    quoted.push_back('\\');
                }
                quoted.push_back(c);
            }
            quoted.push_back('\'');
            return quoted;
        }
        auto make_valid_symbol(const File::StringType& symbol) -> File::StringType
        {
            File::StringType valid;
            valid.reserve(symbol.size());
            auto it = symbol.begin();
            if (it == symbol.end() || std::isdigit(*it))
            {
                valid.push_back('_');
            }
            for (; it != symbol.end(); ++it)
            {
                auto c = *it;
                valid.push_back((c == '_' || std::isalnum(c)) ? c : '_');
            }
            return valid;
        }

      public:
        auto get_file_extension() -> File::StringType
        {
            return STR(".lua");
        }
        auto generate_file_header(GeneratedFile& generated_file) -> void
        {
            generated_file.primary_file.write_string_to_file(STR("---@meta\n\n"));
        }
        auto generate_file_footer(GeneratedFile& generated_file) -> void
        {
        }
        auto generate_enum_declaration(File::StringType& content_buffer, UEnum* uenum) -> void
        {
            auto enum_name = uenum->GetName();
            content_buffer.append(fmt::format(STR("---@enum {}\nlocal {} = {{\n"), enum_name, enum_name));
        }
        auto generate_enum_member(File::StringType& content_buffer, UEnum* uenum, const File::StringType& enum_value_name, const Unreal::FEnumNamePair& elem) -> void
        {
            content_buffer.append(fmt::format(STR("{}{} = {},\n"), generate_tab(), enum_value_name, elem.Value));
        }
        auto generate_enum_end(File::StringType& content_buffer, UEnum* uenum) -> void
        {
            content_buffer.append(STR("}"));
        }

        auto should_generate_class(UStruct* native_class)
        {
            // skip UObject to define externally
            return native_class != UObject::StaticClass();
        }
        auto generate_class(TypeGenerator<LuaTypesGenerator>* generator, ObjectInfo& object_info, GeneratedFile& generated_file, File::StringType& current_class_content)
        {
            UStruct* native_class = static_cast<UStruct*>(object_info.object);
            File::StringType content_buffer{};

            UStruct* inherits_from_class = native_class->GetSuperStruct();

            // Make sure that the base class is defined
            generator->generate_class_dependency(object_info, inherits_from_class, current_class_content);

            // If any properties have dependencies, make sure that they are defined
            // This makes sure that we don't have member variables with undefined types (if the types are local, otherwise we need to include the file that the struct exists in)
            std::vector<PropertyInfo> properties_to_generate{};
            for (XProperty* property : Unreal::TFieldRange<XProperty>(native_class, EFieldIterationFlags::IncludeDeprecated))
            {
                properties_to_generate.emplace_back(
                        PropertyInfo{property, generator->generate_class_dependency_from_property(object_info, property, current_class_content)});
            }

            std::vector<FunctionInfo> functions_to_generate{};
            for (UFunction* function : Unreal::TFieldRange<UFunction>(native_class, EFieldIterationFlags::None))
            {
                auto& function_info = functions_to_generate.emplace_back(FunctionInfo{function, object_info});

                for (XProperty* param : Unreal::TFieldRange<XProperty>(function, EFieldIterationFlags::IncludeDeprecated))
                {
                    if (!param->HasAnyPropertyFlags(Unreal::CPF_Parm | Unreal::CPF_ReturnParm))
                    {
                        continue;
                    }

                    function_info.params.emplace_back(
                            PropertyInfo{param, generator->generate_class_dependency_from_property(object_info, param, current_class_content)});
                }
            }

            auto class_name = generate_class_name(native_class);

            generate_class_declaration(content_buffer, native_class, inherits_from_class);

            int32_t num_padding_elements{0};
            XProperty* last_property_in_this_class{nullptr};

            for (const auto& property_info : properties_to_generate)
            {
                XProperty* property = property_info.property;
                int32_t current_property_offset = property->GetOffset_Internal();
                int32_t current_property_size = property->GetSize();

                try
                {
                    const auto& property_name = property->GetName();
                    if (is_valid_lua_symbol(property_name))
                    {
                        content_buffer.append(fmt::format(STR("---@field {} {}\n"), property_name, generate_property_lua_name(property, true, native_class)));
                    }
                    else
                    {
                        content_buffer.append(
                                fmt::format(STR("---@field [{}] {}\n"), quote_lua_symbol(property_name), generate_property_lua_name(property, true, native_class)));
                    }
                }
                catch (std::exception& e)
                {
                    Output::send<LogLevel::Warning>(STR("Could not generate property '{}' because: {}\n"), property->GetFullName(), ensure_str(e.what()));
                    continue;
                }

                // TODO: Lua delegates

                last_property_in_this_class = property;
            }

            int32_t class_size = native_class->GetPropertiesSize();
            generate_class_struct_end(content_buffer, class_name, class_size, num_padding_elements, last_property_in_this_class);

            // Functions
            if (native_class->HasChildren())
            {
                content_buffer.append(STR("\n"));
                for (const auto& function_info : functions_to_generate)
                {
                    generator->generate_function_declaration(object_info, function_info, generated_file, content_buffer);
                }
            }

            generate_class_end(content_buffer, class_size);

            content_buffer.append(STR("\n\n"));

            current_class_content.append(content_buffer);
        }
        auto generate_class_declaration(File::StringType& content_buffer, UStruct* native_class, UStruct* inherits_from_class) -> void
        {
            auto class_name = generate_class_name(native_class);
            if (inherits_from_class)
            {
                content_buffer.append(fmt::format(STR("---@class {} : {}\n"), class_name, generate_class_name(inherits_from_class)));
            }
            else
            {
                content_buffer.append(fmt::format(STR("---@class {}\n"), class_name));
            }
        }
        auto generate_class_struct_end(File::StringType& content_buffer,
                                       const File::StringType& class_name,
                                       size_t class_size,
                                       int32_t num_padding_elements,
                                       XProperty* last_property_in_this_class) -> void
        {
            content_buffer.append(fmt::format(STR("local {} = {{}}\n"), class_name));
        }
        auto generate_class_end(File::StringType& content_buffer, size_t class_size) -> void
        {
        }

        auto generate_function_declaration(TypeGenerator<LuaTypesGenerator>* generator,
                                           File::StringType& current_class_content,
                                           ObjectInfo& owner,
                                           const FunctionInfo& function_info,
                                           File::StringType function_name,
                                           XProperty* return_property,
                                           std::optional<PropertyInfo> return_property_info) -> void
        {
            for (size_t i = 0; i < function_info.params.size(); ++i)
            {
                const auto& param_info = function_info.params[i];
                if (!param_info.property->HasAnyPropertyFlags(Unreal::CPF_ReturnParm))
                {
                    try
                    {
                        auto param_name = param_info.property->GetName();
                        // TODO disambiguate param renames
                        current_class_content.append(fmt::format(STR("---@param {} {}\n"),
                                                                 make_valid_symbol(param_name),
                                                                 generate_property_lua_name(param_info.property, true, function_info.function)));
                    }
                    catch (std::exception& e)
                    {
                        Output::send<LogLevel::Warning>(STR("Could not generate function '{}' because: {}\n"),
                                                        function_info.function->GetFullName(),
                                                        ensure_str(e.what()));
                        return;
                    }
                }
            }

            if (return_property)
            {
                try
                {
                    current_class_content.append(fmt::format(STR("---@return {}\n"), generate_property_lua_name(return_property, true, function_info.function)));
                }
                catch (std::exception& e)
                {
                    Output::send<LogLevel::Warning>(STR("Could not generate function '{}' because: {}\n"),
                                                    function_info.function->GetFullName(),
                                                    ensure_str(e.what()));
                    return;
                }
            }

            auto class_name = generate_class_name(static_cast<UStruct*>(owner.object));

            if (is_valid_lua_symbol(function_name))
            {
                current_class_content.append(fmt::format(STR("function {}:{}("), class_name, function_name));
            }
            else
            {
                // `function MyClass:MyMethod(p1, p2)` is syntactical sugar for `function MyClass.MyMethod(self, p1, p2)`
                current_class_content.append(fmt::format(STR("{}[{}] = function(self, "), class_name, quote_lua_symbol(function_name)));
            }

            for (size_t i = 0; i < function_info.params.size(); ++i)
            {
                const auto& param_info = function_info.params[i];
                if (!param_info.property->HasAnyPropertyFlags(Unreal::CPF_ReturnParm))
                {
                    auto param_name = param_info.property->GetName();
                    // TODO disambiguate param renames
                    current_class_content.append(fmt::format(STR("{}"), make_valid_symbol(param_name)));

                    if (i + 1 < function_info.params.size())
                    {
                        auto* next_param = function_info.params[i + 1].property;
                        if (next_param && (!next_param->HasAnyPropertyFlags(Unreal::CPF_ReturnParm) || i + 2 < function_info.params.size()))
                        {
                            current_class_content.append(STR(", "));
                        }
                    }
                }
            }
            current_class_content.append(STR(") end"));
        }
    };

    auto generate_cxx_headers(const std::filesystem::path directory_to_generate_in) -> void
    {
        TypeGenerator<CXXHeaderGenerator> generator{directory_to_generate_in};
        generator.generate();
    }

    auto generate_lua_types(const std::filesystem::path directory_to_generate_in) -> void
    {
        TypeGenerator<LuaTypesGenerator> generator{directory_to_generate_in};
        generator.generate();
    }
} // namespace RC::UEGenerator
