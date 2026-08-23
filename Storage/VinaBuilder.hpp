#pragma once
#ifndef VINA_BUILDER_H
#define VINA_BUILDER_H

#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>

class VinaBuilder;

class VinaObject {
private:
    std::wstring name;
    std::vector<std::pair<std::wstring, std::wstring>> data; 
    std::vector<std::unique_ptr<VinaObject>> nestedObjects;
    VinaObject* parent;

public:
    VinaObject(const std::wstring& objName, VinaObject* pParent = nullptr)
        : name(objName), parent(pParent) {}

    VinaObject(const VinaObject&) = delete;
    VinaObject& operator=(const VinaObject&) = delete;
    VinaObject(VinaObject&&) noexcept = default;
    VinaObject& operator=(VinaObject&&) noexcept = default;

    void AddData(const std::wstring& key, const std::wstring& value) {
        data.emplace_back(key, value);
    }

    VinaObject* AddObject(const std::wstring& objName) {
        auto newObj = std::make_unique<VinaObject>(objName, this);
        VinaObject* result = newObj.get();
        nestedObjects.push_back(std::move(newObj));
        return result;
    }

    void BuildWString(std::wstringstream& ss, int depth = 0) const {
        std::wstring indent(depth * 4, L' ');
        ss << indent << name << L"{\n";

        bool first = true;


        for (const auto& kv : data) {
            if (!first) ss << L",\n";
            ss << indent << L"    " << kv.first << L" : " << kv.second;
            first = false;
        }

        for (const auto& obj : nestedObjects) {
            if (!first) ss << L",\n";
            obj->BuildWString(ss, depth + 1);
            first = false;
        }

        ss << L'\n' << indent << L"}";
    }

    std::wstring GetWString() const {
        std::wstringstream ss;
        BuildWString(ss);
        return ss.str();
    }

};

class VinaBuilder {
private:
    std::vector<std::unique_ptr<VinaObject>> rootObjects;

public:
    VinaBuilder() = default;
    VinaBuilder(const VinaBuilder&) = delete;
    VinaBuilder& operator=(const VinaBuilder&) = delete;
    VinaBuilder(VinaBuilder&&) noexcept = default;
    VinaBuilder& operator=(VinaBuilder&&) noexcept = default;

    VinaObject* AddObject(const std::wstring& objName) {
        auto newObj = std::make_unique<VinaObject>(objName);
        VinaObject* result = newObj.get();
        rootObjects.push_back(std::move(newObj));
        return result;
    }

    std::wstring GetWString() const {
        std::wstringstream ss;
        for (size_t i = 0; i < rootObjects.size(); ++i) {
            rootObjects[i]->BuildWString(ss);
            if (i < rootObjects.size() - 1) {
                ss << L'\n';
            }
        }
        return ss.str();
    }
    std::wstring GetContent() const {
        return GetWString();
    }
    bool SaveToFile(const std::wstring& filename) const {
        std::wofstream file{ std::filesystem::path(filename) };
        if (!file.is_open()) {
            return false;
        }

        file << GetWString();
        file.flush();
        return file.good();
    }
};

#endif // VINA_BUILDER_H
