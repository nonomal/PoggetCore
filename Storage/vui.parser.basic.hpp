/**
 * @file vui.parser.basic.hpp
 * @author Haceau (haceau@qq.com)
 * @brief vui 解析器的通用基本类。
 * @version 0.2.0 (Modified for Vina syntax)
 * @date 2022-05-11
 * @copyright Copyright (c) 2022
 *
 */
#pragma once
#include <iterator>
#include <utility>
#ifndef VUI_PARSER_H_
#define VUI_PARSER_H_

#include <cctype>
#include <cinttypes>
#include <algorithm>
#include <any>
#include <string>
#include <unordered_map>
#include <locale>
#include <iostream>
#include <optional>
#include <limits>
#include <sstream>
#include <vector>

namespace vui::parser
{
    struct parser_limits
    {
        std::size_t max_input_bytes = 512ULL * 1024 * 1024;
        std::size_t max_depth = 128;
        std::size_t max_objects = 1'000'000;
        std::size_t max_members = 4'000'000;
        std::size_t max_name_length = 64 * 1024;
        std::size_t max_value_length = 64 * 1024 * 1024;
    };

    enum class parser_error
    {
        none,
        syntax,
        resource_limit
    };

    ///
    /// @class basic_value_pair <CharT>
    /// @brief 通用的 vui 键值对类。
    /// @param CharT [类型] 字符类型。
    ///
    template <typename CharT>
    class basic_value_pair
    {
    public:
        /// @brief 字符串类型。
        using string_type = std::basic_string<CharT>;
        /// @brief 原始类型。
        using raw_type = std::pair<string_type const, std::any>;

        /// @brief 初始化器。
        basic_value_pair(raw_type const& pair)
            : pair_(pair) { }

        /// @brief 获取数据。
        /// @param T [可选|类型] 获取的类型，默认为 `string_type`。
        /// @param result [返回] 获取到的数据。若获取失败，`result` 中的内容不改变。
        /// @return 成功返回 `true`，失败返回 `false`。
        template <typename T = string_type>
        bool get(T& result) const
        {
            std::any value{ pair_.second };
            if (value.type() != typeid(T))
                return false;
            result = std::any_cast<T>(value);
            return true;
        }

        /// @brief 获取名称。
        /// @return 名称。
        string_type name() const
        {
            return pair_.first;
        }

    private:
        raw_type pair_;
    };

    ///
    /// @class basic_object <CharT>
    /// @brief 通用的 vui 对象类。
    /// @param CharT 字符类型。
    ///
    template <typename CharT>
    class basic_object
    {
    public:
        /// @brief 对象使用的字符串类型。
        using string_type = std::basic_string<CharT>;
        /// @brief 原始对象类型。
        using object_type = std::unordered_map<string_type, std::any>;

        basic_object() {}
        explicit basic_object(std::pair<string_type, basic_object<CharT>> const& pair)
            : name_(pair.first), obj_(pair.second.obj_)
        {
            name_.erase(std::remove(name_.begin(), name_.end(), '^'),
                name_.end());
        }
        basic_object(std::pair<string_type, basic_object<CharT>> const& pair, std::vector<string_type> const& order)
            : basic_object(pair) {
            order_ = order;
        }
        basic_object(string_type name, basic_object<CharT> const& object)
            : name_(std::move(name)), obj_(object.obj_), order_(object.order_)
        {
            name_.erase(std::remove(name_.begin(), name_.end(), '^'), name_.end());
        }

        /// @brief 对象的迭代器。
        class iterator
        {
            friend class basic_object<CharT>;
        public:
            /// @brief 值类型。
            using value_type = basic_value_pair<CharT>;
            /// @brief 引用类型。
            using reference = value_type;
            /// @brief 指针类型。
            using pointer = value_type*;

            iterator(object_type& objs, std::vector<string_type> const& order)
                : objs_(objs), order_(order) { }
            /// @brief 获取 vui 键值对。
            value_type operator*() {
                string_type name{ order_[pos_] };
                return value_type(typename value_type::raw_type{ std::move(name), objs_.at(order_[pos_]) });
            }
                /// @brief 向前移动一位。
                iterator& operator++() { ++pos_; return *this; }
            /// @brief 向前移动一位，返回移动前的迭代器。
            iterator operator++(int) { auto it{ *this }; ++*this; return it; }
            /// @brief 向后移动一位。
            iterator& operator--() { --pos_; return *this; }
            /// @brief 向后移动一位，返回移动前的迭代器。
            iterator operator--(int) { auto it{ *this }; --*this; return it; }
            /// @brief 判断两个迭代器是否相等。
            bool operator==(iterator const& other) { return &objs_ == &other.objs_ && pos_ == other.pos_; }
            /// @brief 判断两个迭代器是否不相等。
            bool operator!=(iterator const& other) { return !(*this == other); }
            /// @brief 访问对象。
            pointer operator->() {
                current_value_.emplace(**this);
                return &current_value_.value();
            }

        private:
            object_type& objs_;
            std::vector<string_type> const& order_;
            std::size_t pos_{ 0 };
            std::optional<value_type> current_value_;
        };

        /// @brief 获取对象的起始迭代器。
        /// @return 对象的起始迭代器。
        iterator begin()
        {
            return { obj_, order_ };
        }

        /// @brief 获取对象的结束迭代器。
        /// @return 对象的结束迭代器。
        iterator end()
        {
            iterator it(obj_, order_);
            it.pos_ = order_.size();
            return it;
        }

        /// @brief 获取对象中键为 `key` 的值的个数。
        /// @param key 要获取的键。
        auto count(string_type const& key) const
        {
            return obj_.count(key);
        }

        /// @brief 获取对象名。
        string_type name() const { return name_; };

        /// @brief 访问对象。
        /// @param key 要访问的键。
        std::any const& operator[](string_type const& key) const
        {
            return obj_.at(key);
        }

        /// @brief 访问对象。
        /// @param key 要访问的键。
        std::any& operator[](string_type const& key)
        {
            return obj_[key];
        }

        /// @brief 添加对象。
        void add(string_type const& key, std::any& value)
        {
            if (obj_.find(key) == obj_.end()) order_.emplace_back(key);
            obj_[key] = value;
        }

        /// @brief 添加对象（右值引用）。
        void add(string_type const& key, std::any&& value)
        {
            if (obj_.find(key) == obj_.end()) order_.emplace_back(key);
            obj_[key] = std::move(value);
        }

        /// @brief 获取数据顺序。
        std::vector<string_type> const& order() const
        {
            return order_;
        }

        /// @brief 判断两个对象是否相等。
        /// @param other 要判断的对象。
        /// @return 相等返回 `true`，不相等返回 `false`。
        bool operator==(object_type const& other) { return name_ == other.name_ && obj_ == other.obj_; }
        /// @brief 判断两个对象是否不相等。
        /// @param other 要判断的对象。
        /// @return 相等返回 `false`，不相等返回 `true`。
        bool operator!=(object_type const& other) { return !(*this == other); }

    private:
        string_type name_;
        object_type obj_;
        std::vector<string_type> order_;

    public:
        /// @brief 初始化器。
        basic_object(object_type&& obj)
            : obj_(std::move(obj)) { }
    };

    ///
    /// @class basic_parser <StreamT, ArgT, CharT> 
    /// @brief 通用的 vui 格式解析类。
    /// @param StreamT 流类型。
    /// @param CharT 字符类型。
    ///
    template <typename StreamT, typename CharT>
    class basic_parser
    {
    public:

        /// @brief 解析器使用的字符串类型。
        using string_type = std::basic_string<CharT>;
        /// @brief 解析器使用的对象类型。
        using object_type = basic_object<CharT>;
        /// @brief 对象表。
        using objects_type = std::unordered_map<string_type, object_type>;

        /// @brief 初始化器。
    /// @param s 要解析的流。
        template<typename T>
        basic_parser(T const& s) // 保留旧的 const& 构造函数
            : stream_(s) { } // 对于可拷贝的类型，这仍然是有效的

        /// @brief 初始化器。
        /// @param s 要解析的流。
        template<typename T>
        basic_parser(T&& s) // 新增移动构造函数
            : stream_(std::forward<T>(s)) { } // 使用 std::forward 处理左值和右值

        /// @brief 初始化器。
        /// @param s 要解析的流。
        /// @param region 要解析的 region。
        template<typename T>
        basic_parser(T const& s, string_type const& region)
            : stream_(s)
            , region_(region) { }

        /// @brief 初始化器。
        /// @param s 要解析的流。
        /// @param region 要解析的 region。
        template<typename T>
        basic_parser(T&& s, string_type const& region)
            : stream_(std::forward<T>(s))
            , region_(region) { }

        /// @brief 设置 region。
        /// @param region 要解析的 region。
        void set_region(string_type const& region) { region_ = region; }
        /// @brief 获取 region。
        /// @return 要解析的的 region。
        string_type const& region() const noexcept { return region_; }

        void set_limits(parser_limits limits) noexcept { limits_ = limits; }
        parser_limits const& limits() const noexcept { return limits_; }
        parser_error error() const noexcept { return error_; }

        /// @brief 获取数据。
        /// @param key 要获取的数据的名称。
        /// @param result [返回] 获取到的数据。若获取失败，`result` 中的内容不改变。
        /// @param name [可选] 要获取数据的对象的名称。默认为解析的第一个对象。
        /// @return 成功返回 `true`，失败返回 `false`。
        ///
        /// 若在 `get` 前未进行过 `parse`，将会自动执行一次 `parse`。
        /// 同名对象可使用 `same_name_object` 函数进行名称处理。
        template <typename T = string_type>
        bool get(string_type const& key, T& result, std::optional<string_type> const& name = std::nullopt)
        {
            if (!objs_.has_value() && !parse()) return false;
            if (!objs_.has_value() || objs_->empty()) return false;
            auto& objs{ objs_.value() };
            const object_type* obj = nullptr;
            if (!name.has_value()) {
                obj = &objs.begin()->second;
            }
            else {
                auto it = objs.find(name.value());
                if (it == objs.end()) return false;
                obj = &it->second;
            }

            if (!obj->count(key))
                return false;
            const std::any& value = (*obj)[key];
            if (value.type() != typeid(T))
                return false;
            result = std::any_cast<T>(value);
            return true;
        }
        template <typename T = string_type>
        bool get2(string_type const& key, T& result, std::optional<string_type> const& name = std::nullopt)
        {
            if (!objs_.has_value() || objs_->empty()) return false;
            auto& objs{ objs_.value() };
            const object_type* obj = nullptr;
            if (!name.has_value()) {
                obj = &objs.begin()->second;
            }
            else {
                auto it = objs.find(name.value());
                if (it == objs.end()) return false;
                obj = &it->second;
            }

            if (!obj->count(key))
                return false;
            const std::any& value = (*obj)[key];
            if (value.type() != typeid(T))
                return false;
            result = std::any_cast<T>(value);
            return true;
        }
        /// @brief 解析流中的数据。
        /// @return 成功返回 `true`，失败返回 `false`。
        /// 
        /// 只能 `parse` 一次，多次 `parse` 将返回 `false`。
        /// 对象名禁用 「^」。
        bool parse()
        {
            objs_ = objects_type{};
            order_.clear();
            error_ = parser_error::none;
            object_count_ = 0;
            member_count_ = 0;
            stream_ >> std::noskipws;

            CharT c{};
            if (!read_non_whitespace(c)) return fail(parser_error::syntax);
            if (c == static_cast<CharT>('#')) return parse_preprocessor();

            do {
                if (!parse_object(c)) {
                    objs_.reset();
                    order_.clear();
                    return fail(error_ == parser_error::none ? parser_error::syntax : error_);
                }
            } while (read_non_whitespace(c));

            return objs_.has_value() && !objs_->empty();
        }

        /// @brief 对象的迭代器。
        class iterator
        {
            friend class basic_parser<StreamT, CharT>;
        public:
            /// @brief 值类型。
            using value_type = object_type;
            /// @brief 引用类型。
            using reference = value_type;
            /// @brief 指针类型。
            using pointer = value_type*;

            /// @brief 获取 vui 对象。
            value_type operator*() {
                const string_type& name = order_[pos_];
                const auto& object = objs_.at(name);
                return value_type(name, object);
            }
                /// @brief 向前移动一位。
                iterator& operator++() { ++pos_; return *this; }
            /// @brief 向前移动一位，返回移动前的迭代器。
            iterator operator++(int) { auto it{ *this }; ++*this; return it; }
            /// @brief 向后移动一位。
            iterator& operator--() { --pos_; return *this; }
            /// @brief 向后移动一位，返回移动前的迭代器。
            iterator operator--(int) { auto it{ *this }; --*this; return it; }
            /// @brief 判断两个迭代器是否相等。
            bool operator==(iterator const& other) { return &objs_ == &other.objs_ && pos_ == other.pos_; }
            /// @brief 判断两个迭代器是否不相等。
            bool operator!=(iterator const& other) { return !(*this == other); }
            /// @brief 访问对象。
            pointer operator->() {
                current_value_.emplace(**this);
                return &current_value_.value();
            }

            iterator(std::unordered_map<string_type, object_type>& objs, std::vector<string_type> const& order)
                : objs_(objs), order_(order) { }

        private:
            std::unordered_map<string_type, object_type>& objs_;
            std::vector<string_type> const& order_;
            std::size_t pos_{ 0 };
            std::optional<value_type> current_value_;

            bool end() { return pos_ >= order_.size(); }
        };

        /// @brief 获取对象的起始迭代器。
        /// @return 对象的起始迭代器。
        iterator begin()
        {
            return { objs_.value(), order_ };
        }

        /// @brief 获取对象的结束迭代器。
        /// @return 对象的结束迭代器。
        iterator end()
        {
            iterator it(objs_.value(), order_);
            it.pos_ = it.order_.size();
            return it;
        }

    protected:
        StreamT stream_;
        std::optional<objects_type> objs_;
        std::vector<string_type> order_;
        string_type region_;
        parser_limits limits_{};
        parser_error error_{ parser_error::none };
        std::size_t object_count_{ 0 };
        std::size_t member_count_{ 0 };

        bool fail(parser_error value) noexcept
        {
            if (error_ == parser_error::none) error_ = value;
            return false;
        }

        static bool exceeds(std::size_t value, std::size_t limit) noexcept
        {
            return limit != 0 && value > limit;
        }

        bool parse_preprocessor()
        {
            if (region_.empty()) return true;

            CharT c{};
            bool is_region = false;
            while (!is_region)
            {
                if (!(stream_ >> c) || c != '#') return false;
                bool conti = false;
                for (auto const& reg : region_)
                    if (!(stream_ >> c) || c != reg)
                    {
                        skip_to('#');
                        for (std::size_t i = 0; i < 3; ++i)
                            if (!(stream_ >> c) || c != '#') return false;
                        conti = true;
                        break;
                    }
                if (conti) continue;
                is_region = true;
            }
            c = skip_whitespace();
            while (c != '#' && (!stream_.eof()))
            {
                if (!parse_object(c)) return false;
                stream_ >> c;
            }
            if (stream_.eof()) return false;
            if (!(stream_ >> c) || c != '#') return false;
            if (!(stream_ >> c) || c != '#') return false;
            return true;
        }

        bool parse_members(object_type& obj, std::size_t depth)
        {
            if (exceeds(depth, limits_.max_depth)) return fail(parser_error::resource_limit);
            CharT c{};
            if (!read_non_whitespace(c)) return fail(parser_error::syntax);
            while (c != static_cast<CharT>('}'))
            {
                /// 逗号处理
                if (c == static_cast<CharT>(',')) {
                    if (!read_non_whitespace(c)) return false;
                    if (c == static_cast<CharT>('}')) return true;
                }

                /// 读取 Key
                string_type key;
                /// 读取直到遇到分隔符 (、:、{ 或空白
                while (c != static_cast<CharT>('(') && c != static_cast<CharT>(':') &&
                    c != static_cast<CharT>('{') && !is_space(c) &&
                    c != static_cast<CharT>(',') && c != static_cast<CharT>('}')) {
                    key += c;
                    if (exceeds(key.size(), limits_.max_name_length)) return fail(parser_error::resource_limit);
                    if (!stream_.get(c)) return fail(parser_error::syntax);
                }
                if (key.empty()) return fail(parser_error::syntax);

                /// 跳过键名和分隔符之间的空白
                while (is_space(c)) {
                    if (!stream_.get(c)) return fail(parser_error::syntax);
                }

                if (obj.count(key)) return fail(parser_error::syntax);
                if (exceeds(++member_count_, limits_.max_members)) return fail(parser_error::resource_limit);

                CharT separator = c;
                std::any value;

                if (separator == static_cast<CharT>('(')) {
                    if (!read_value(value, c, [](CharT x) { return x == static_cast<CharT>(')'); })) return false;
                    if (!read_non_whitespace(c)) return fail(parser_error::syntax);
                }
                else if (separator == static_cast<CharT>(':')) {
                    if (!read_value(value, c, [](CharT x) {
                        return x == static_cast<CharT>(',') || x == static_cast<CharT>('}');
                    })) return false;
                }
                else if (separator == static_cast<CharT>('{')) {
                    object_type nested_obj;
                    if (exceeds(++object_count_, limits_.max_objects)) return fail(parser_error::resource_limit);
                    if (!parse_members(nested_obj, depth + 1)) return false;
                    value = std::move(nested_obj);
                    if (!read_non_whitespace(c)) return fail(parser_error::syntax);
                }
                else {
                    return fail(parser_error::syntax);
                }

                obj.add(key, std::move(value));
            }
            return true;
        }

        bool parse_object(CharT c)
        {
            string_type name{ c };
            if (!read_to(static_cast<CharT>('{'), name, limits_.max_name_length)) return false;

            while (!name.empty() && is_space(name.back())) name.pop_back();
            if (name.empty()) return fail(parser_error::syntax);

            object_type obj;
            if (exceeds(++object_count_, limits_.max_objects)) return fail(parser_error::resource_limit);
            if (!parse_members(obj, 1)) return false;

            if (objs_->count(name)) return fail(parser_error::syntax);
            objs_->emplace(name, std::move(obj));
            if (name.front() != static_cast<CharT>('@'))
                order_.emplace_back(std::move(name));
            return true;
        }

        CharT skip_whitespace()
        {
            CharT c{};
            while (stream_.get(c) && is_space(c));
            return c;
        }

        bool read_non_whitespace(CharT& c)
        {
            while (stream_.get(c)) {
                if (!is_space(c)) return true;
            }
            return false;
        }

        void skip_to(CharT end)
        {
            CharT c{};
            while (stream_.get(c) && c != end);
        }

        bool read_to(CharT end, string_type& out, std::size_t max_length)
        {
            CharT c{};
            while (stream_.get(c)) {
                if (c == end) return true;
                out += c;
                if (exceeds(out.size(), max_length)) return fail(parser_error::resource_limit);
            }
            return fail(parser_error::syntax);
        }
        static bool is_space(CharT c) noexcept
        {
            return c == static_cast<CharT>(' ') || c == static_cast<CharT>('\t') ||
                c == static_cast<CharT>('\n') || c == static_cast<CharT>('\r') ||
                c == static_cast<CharT>('\f') || c == static_cast<CharT>('\v');
        }

        static bool is_digit(CharT c) noexcept
        {
            return c >= static_cast<CharT>('0') && c <= static_cast<CharT>('9');
        }

        static bool parse_integer(const string_type& text, int& result) noexcept
        {
            if (text.empty()) return false;

            std::size_t pos = 0;
            bool negative = false;
            if (text[pos] == static_cast<CharT>('-') || text[pos] == static_cast<CharT>('+')) {
                negative = text[pos] == static_cast<CharT>('-');
                if (++pos == text.size()) return false;
            }

            const std::uintmax_t limit = negative
                ? static_cast<std::uintmax_t>(-(std::numeric_limits<int>::min() + 1)) + 1
                : static_cast<std::uintmax_t>(std::numeric_limits<int>::max());
            std::uintmax_t value = 0;
            for (; pos < text.size(); ++pos) {
                if (!is_digit(text[pos])) return false;
                const unsigned digit = static_cast<unsigned>(text[pos] - static_cast<CharT>('0'));
                if (value > (limit - digit) / 10) return false;
                value = value * 10 + digit;
            }

            if (negative) {
                result = value == limit
                    ? std::numeric_limits<int>::min()
                    : -static_cast<int>(value);
            }
            else {
                result = static_cast<int>(value);
            }
            return true;
        }

        static bool has_decimal_syntax(const string_type& text) noexcept
        {
            if (text.empty()) return false;

            std::size_t pos = 0;
            if (text[pos] == static_cast<CharT>('-') || text[pos] == static_cast<CharT>('+')) {
                if (++pos == text.size()) return false;
            }

            bool has_digit = false;
            bool has_fraction_or_exponent = false;
            while (pos < text.size() && is_digit(text[pos])) {
                has_digit = true;
                ++pos;
            }
            if (pos < text.size() && text[pos] == static_cast<CharT>('.')) {
                has_fraction_or_exponent = true;
                ++pos;
                while (pos < text.size() && is_digit(text[pos])) {
                    has_digit = true;
                    ++pos;
                }
            }
            if (!has_digit) return false;

            if (pos < text.size() &&
                (text[pos] == static_cast<CharT>('e') || text[pos] == static_cast<CharT>('E'))) {
                has_fraction_or_exponent = true;
                ++pos;
                if (pos < text.size() &&
                    (text[pos] == static_cast<CharT>('-') || text[pos] == static_cast<CharT>('+'))) {
                    ++pos;
                }
                const std::size_t exponent_start = pos;
                while (pos < text.size() && is_digit(text[pos])) ++pos;
                if (pos == exponent_start) return false;
            }

            return has_fraction_or_exponent && pos == text.size();
        }

        static bool parse_decimal(const string_type& text, double& result)
        {
            if (!has_decimal_syntax(text)) return false;
            std::basic_istringstream<CharT> input(text);
            input.imbue(std::locale::classic());
            input >> std::noskipws >> result;
            if (!input) return false;
            CharT extra{};
            return !input.get(extra);
        }

        template <typename EndPredicate>
        bool read_value(std::any& out, CharT& c, EndPredicate&& is_end)
        {
            if (!read_non_whitespace(c)) return false;

            if (c == static_cast<CharT>('"')) {
                string_type value;
                while (stream_.get(c)) {
                    if (c == static_cast<CharT>('"')) {
                        while (stream_.get(c)) {
                            if (is_end(c)) {
                                out = std::move(value);
                                return true;
                            }
                            if (!is_space(c)) return false;
                        }
                        return false;
                    }

                    if (c == static_cast<CharT>('\\')) {
                        CharT escaped{};
                        if (!stream_.get(escaped)) return false;
                        if (escaped == static_cast<CharT>('\\') || escaped == static_cast<CharT>('"')) {
                            value += escaped;
                        }
                        else {
                            value += static_cast<CharT>('\\');
                            value += escaped;
                        }
                    }
                    else {
                        value += c;
                    }
                    if (exceeds(value.size(), limits_.max_value_length)) return fail(parser_error::resource_limit);
                }
                return fail(parser_error::syntax);
            }

            string_type value;
            while (true) {
                if (is_end(c)) break;
                if (is_space(c)) {
                    do {
                        if (!stream_.get(c)) return false;
                    } while (is_space(c));
                    if (!is_end(c)) return false;
                    break;
                }
                value += c;
                if (exceeds(value.size(), limits_.max_value_length)) return fail(parser_error::resource_limit);
                if (!stream_.get(c)) return false;
            }

            if (value.empty()) {
                out = string_type{};
                return true;
            }
            if (value == string_type{ static_cast<CharT>('t'), static_cast<CharT>('r'),
                    static_cast<CharT>('u'), static_cast<CharT>('e') }) {
                out = true;
                return true;
            }
            if (value == string_type{ static_cast<CharT>('f'), static_cast<CharT>('a'),
                    static_cast<CharT>('l'), static_cast<CharT>('s'), static_cast<CharT>('e') }) {
                out = false;
                return true;
            }

            int integer_value = 0;
            if (parse_integer(value, integer_value)) {
                out = integer_value;
                return true;
            }

            double decimal_value = 0.0;
            if (parse_decimal(value, decimal_value)) {
                out = decimal_value;
                return true;
            }

            out = std::move(value);
            return true;
        }

        /// 兼容旧read_string
        bool read_string(string_type& out, bool& flag)
        {
            CharT c{ };
            stream_ >> c;
            while (!stream_.eof())
            {
                if (c == '"') {
                    // 计算 out 末尾连续反斜杠的数量
                    size_t bs_count = 0;
                    for (auto rit = out.rbegin(); rit != out.rend() && *rit == '\\'; ++rit) {
                        ++bs_count;
                    }
                    if (out.empty() || bs_count % 2 == 0) {
                        // 偶数个反斜杠：" 是真正的结束引号
                        stream_ >> c;
                        if (c == ')') { flag = true; break; }
                        else {
                            // 不是 )，说明这个引号后面还有内容
                            // 保留原有行为
                            out += '"';
                            continue; // c 已经是下一个字符了
                        }
                    }
                    else {
                        // 奇数个反斜杠：" 是被转义的
                        out.back() = '"';
                        stream_ >> c;
                        continue;
                    }
                }
                else {
                    out += c;
                }
                stream_ >> c;
            }
            return !stream_.eof();
        }
    };

    /// @brief 为同名对象创建标识符。
    /// @param object_name 对象名。
    /// @param id 同名对象的唯一标识符。
    /// @param split 对象名与标识符的分隔符。默认为 `:`。
    template<typename C>
    std::basic_string<C> same_name_object(std::basic_string<C> object_name, std::basic_string<C> id, C split = ':')
    {
        return object_name + split + id;
    }


    /// @brief 判断是否为虚对象。
    /// @param C [类型] 字符类型。
    /// @param object_name 对象名。
    /// @return 是虚对象返回 `true`，不是返回 `false`。
    /// 
    /// 以「$」或「@」开头的对象为「虚对象」。
    template<typename C>
    bool is_virtual_object(std::basic_string<C> object_name)
    {
        return !object_name.empty() && object_name[0] == '@';
    }
}

#endif // VUI_PARSER_H_
