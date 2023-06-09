/*
This code has been adapted from MIT licensed code, originally by Jeremy burns and available at
https://gist.github.com/jaburns/ca72487198832f6203e831133ffdfff4.
The original license is provided below:
Copyright 2021 Jeremy Burns
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files
(the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge,
publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:
The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include <functional>
#include <vector>
#include <cstdint>
#include <optional>
#include <limits>
#include <stdexcept>
#include <string>
#include <fstream>

struct GenerationalIndex {
    uint32_t index = 0;
    uint32_t generation = 0;

    bool operator<(const GenerationalIndex& other) const { 
        if(generation == other.generation)
            return index < other.index;
        else 
            return generation < other.generation; 
    }
    
    bool operator==(const GenerationalIndex& other) const { 
        return index == other.index && generation == other.generation; 
    }
};
namespace std{
    template<> struct std::tr1::hash<GenerationalIndex>{
        size_t operator()(const GenerationalIndex &GI) const{
            return (hash<uint32_t>()(GI.index) ^ hash<uint32_t>()(GI.generation));
        }
    };
}

std::ostream& operator<<(std::ostream &os, GenerationalIndex ind)
{
    os << "Generation: " << ind.generation << "Index: " << ind.index << std::endl;
    return os;
}
class GenerationalIndexAllocator
{
    struct AllocatorEntry {
        bool isLive = false;
        uint32_t generation = 0;
    };

    std::vector<AllocatorEntry> m_entries;
    std::vector<uint32_t> m_freeIndices;

public:
    GenerationalIndex allocate()
    {
        if (m_freeIndices.size() > 0) {
            uint32_t index = m_freeIndices.back();
            m_freeIndices.pop_back();

            m_entries[index].generation += 1;
            m_entries[index].isLive = true;

            return { index, m_entries[index].generation };
        } else {
            // check that we are still within the bounds of uint32_t
            if (m_entries.size() + 1 >= std::numeric_limits<uint32_t>::max()) {
                throw std::length_error(std::string("Maximum number of values inside GenerationalIndexArray reached: ") + std::to_string(m_entries.size()));
            }

            m_entries.push_back({ true, 0 });
            return { static_cast<uint32_t>(m_entries.size()) - 1, 0 };
        }
    }

    bool deallocate(GenerationalIndex index)
    {
        if (isLive(index)) {
            m_entries[index.index].isLive = false;
            m_freeIndices.emplace_back(index.index);
            return true;
        }

        return false;
    }

    bool isLive(GenerationalIndex index) const noexcept
    {
        return index.index < m_entries.size() &&
                m_entries[index.index].generation == index.generation &&
                m_entries[index.index].isLive;
    }
};

// A GenerationalIndexArray stores elements in contiguous memory just like an std::vector
// and also allows items to be retrieved in constant time through indexed access, but it keeps
// track of the "version"/generation of values at indices so that it can inform an accessor
// when the item at the index it is trying to access is no longer the item that it wants.
template<typename T>
class GenerationalIndexArray
{
    struct Entry {
        uint32_t generation;
        T value;
    };

    // TODO: m_entries never shrinks after an entry has been deleted, it might be
    // a good idea to add a "trim" function at some point if this becomes an issue

    std::vector<std::optional<Entry>> m_entries;
    GenerationalIndexAllocator m_allocator;

    
public:

    T& operator[](GenerationalIndex index)
    {
        return *get(index);
    }

    const T& operator[](const GenerationalIndex index) const
    {
        return *get(index);
    }

    // Sets the value at a specific index inside the array
    void set(const GenerationalIndex index, T &&value)
    {
        while (m_entries.size() <= index.index)
            m_entries.emplace_back(std::nullopt);

        m_entries[index.index] = std::optional<Entry>{ { index.generation, std::move(value) } };
    }

    // Insert a value at the first free index and get the index back
    GenerationalIndex insert(T &&value)
    {
        const auto index = m_allocator.allocate();
        set(index, std::move(value));
        return index;
    }

    // Erase the value at the specified index and free up the index again
    void erase(GenerationalIndex index)
    {
        if (m_allocator.deallocate(index))
            m_entries[index.index] = std::nullopt;
    }

    // Get a pointer to the value at the specified index
    T *get(GenerationalIndex index)
    {
        if (index.index >= m_entries.size())
            return nullptr;

        auto &entry = m_entries[index.index];
        if (entry && entry->generation == index.generation) {
            return &entry->value;
        }

        return nullptr;
    }

    // Get a const pointer to the value at the specified index
    const T *get(GenerationalIndex index) const noexcept
    {
        return const_cast<const T *>(const_cast<GenerationalIndexArray *>(this)->get(index));
    }

    // Erase all the values in the array and thus free up all indices too
    void clear()
    {
        const auto numEntries = entriesSize();

        for (auto i = decltype(numEntries){ 0 }; i < numEntries; ++i) {
            const auto index = indexAtEntry(i);

            if (index != std::nullopt)
                erase(*index);
        }
    }

    // The number entries currently in the array, not all necessarily correspond to valid indices,
    // use "indexAtEntry" to translate from an entry index to a optional GenerationalIndex
    uint32_t entriesSize() const noexcept
    {
        // this cast is safe because the allocator checks that we never exceed the capacity of uint32_t
        return static_cast<uint32_t>(m_entries.size());
    }

    const std::vector<T> get_all_valid_values() const {
        std::vector<T> valid_values;
        valid_values.reserve(m_entries.size()); 
        for(auto&& entry : m_entries)
        {
            if(entry) valid_values.emplace_back(entry->value);
        }
        return valid_values;
    }

    bool is_live(GenerationalIndex index)
    {
        if (m_allocator.isLive(index)) return true;
        else return false;
    }
    // Convert an entry index into a GenerationalIndex, if possible otherwise returns nullopt
    std::optional<GenerationalIndex> indexAtEntry(uint32_t entryIndex) const
    {
        if (entryIndex >= entriesSize())
            return std::nullopt;

        const auto &entry = m_entries[entryIndex];
        if (!entry)
            return std::nullopt;

        GenerationalIndex index = { entryIndex, entry->generation };

        if (m_allocator.isLive(index))
            return index;

        return std::nullopt;
    }
};
