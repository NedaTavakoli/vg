#include "minimizer.hpp"

#include <algorithm>
#include <iostream>

namespace vg {

//------------------------------------------------------------------------------

const std::vector<unsigned char> MinimizerIndex::CHAR_TO_PACK = {
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,

    4, 0, 4, 1,  4, 4, 4, 2,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  3, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 0, 4, 1,  4, 4, 4, 2,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  3, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,

    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,

    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4
};

const std::vector<char> MinimizerIndex::PACK_TO_CHAR = { 'A', 'C', 'G', 'T' };

const std::vector<MinimizerIndex::key_type> MinimizerIndex::KMER_MASK = {
    0x0000000000000000ull,
    0x0000000000000003ull,
    0x000000000000000Full,
    0x000000000000003Full,
    0x00000000000000FFull,
    0x00000000000003FFull,
    0x0000000000000FFFull,
    0x0000000000003FFFull,
    0x000000000000FFFFull,
    0x000000000003FFFFull,
    0x00000000000FFFFFull,
    0x00000000003FFFFFull,
    0x0000000000FFFFFFull,
    0x0000000003FFFFFFull,
    0x000000000FFFFFFFull,
    0x000000003FFFFFFFull,
    0x00000000FFFFFFFFull,
    0x00000003FFFFFFFFull,
    0x0000000FFFFFFFFFull,
    0x0000003FFFFFFFFFull,
    0x000000FFFFFFFFFFull,
    0x000003FFFFFFFFFFull,
    0x00000FFFFFFFFFFFull,
    0x00003FFFFFFFFFFFull,
    0x0000FFFFFFFFFFFFull,
    0x0003FFFFFFFFFFFFull,
    0x000FFFFFFFFFFFFFull,
    0x003FFFFFFFFFFFFFull,
    0x00FFFFFFFFFFFFFFull,
    0x03FFFFFFFFFFFFFFull,
    0x0FFFFFFFFFFFFFFFull,
    0x3FFFFFFFFFFFFFFFull
};

MinimizerIndex::Header::Header() :
    tag(TAG), version(VERSION),
    flags(0),
    k(KMER_LENGTH), w(WINDOW_LENGTH),
    keys(0), capacity(INITIAL_CAPACITY), max_keys(INITIAL_CAPACITY * MAX_LOAD_FACTOR),
    values(0), max_values(MAX_VALUES),
    unique(0), frequent(0)
{
}

MinimizerIndex::Header::Header(size_t kmer_length, size_t window_length, size_t max_values_per_key) :
    tag(TAG), version(VERSION),
    flags(0),
    k(kmer_length), w(window_length),
    keys(0), capacity(INITIAL_CAPACITY), max_keys(INITIAL_CAPACITY * MAX_LOAD_FACTOR),
    values(0), max_values(max_values_per_key),
    unique(0), frequent(0)
{
    this->sanitize();
}

void MinimizerIndex::Header::sanitize() {
    if (this->k > KMER_MAX_LENGTH) {
        std::cerr << "warning: [MinimizerIndex] Adjusting k from " << this->k << " to " << KMER_MAX_LENGTH << std::endl;
        this->k = KMER_MAX_LENGTH;
    }
    if (this->k == 0) {
        std::cerr << "warning: [MinimizerIndex] Adjusting k from " << this->k << " to " << 1 << std::endl;
        this->k = 1;
    }

    if (this->w == 0) {
        std::cerr << "warning: [MinimizerIndex] Adjusting w from " << this->w << " to " << 1 << std::endl;
        this->w = 1;
    }

    if (this->max_values == 0) {
        std::cerr << "warning: [MinimizerIndex] Adjusting max_values from " << this->max_values << " to " << 1 << std::endl;
        this->max_values = 1;
    }
}

bool MinimizerIndex::Header::check() const {
    return (this->tag == TAG && this->version >= MIN_VERSION && this->version <= VERSION && this->flags == 0);
}

//------------------------------------------------------------------------------

MinimizerIndex::MinimizerIndex() :
    header(),
    hash_table(this->header.capacity, empty_cell()),
    is_pointer(this->header.capacity, false)
{
}

MinimizerIndex::MinimizerIndex(size_t kmer_length, size_t window_length,  size_t max_values_per_key) :
    header(kmer_length, window_length, max_values_per_key),
    hash_table(this->header.capacity, empty_cell()),
    is_pointer(this->header.capacity, false)
{
}

MinimizerIndex::MinimizerIndex(const MinimizerIndex& source) {
    this->copy(source);
}

MinimizerIndex::MinimizerIndex(MinimizerIndex&& source) {
    *this = std::move(source);
}

MinimizerIndex::~MinimizerIndex() {
    this->clear();
}

void MinimizerIndex::swap(MinimizerIndex& another) {
    if (&another == this) {
        return;
    }

    std::swap(this->header, another.header);
    this->hash_table.swap(another.hash_table);
    this->is_pointer.swap(another.is_pointer);
}

MinimizerIndex& MinimizerIndex::operator=(const MinimizerIndex& source) {
    if (&source != this) {
        this->copy(source);
    }
    return *this;
}

MinimizerIndex& MinimizerIndex::operator=(MinimizerIndex&& source) {
    if (&source != this) {
        this->header = std::move(source.header);
        this->hash_table = std::move(source.hash_table);
        this->is_pointer = std::move(source.is_pointer);
    }
    return *this;
}

void MinimizerIndex::copy(const MinimizerIndex& source) {
    this->clear();
    this->header = source.header;
    this->hash_table = source.hash_table;
    this->is_pointer = source.is_pointer;
}

void MinimizerIndex::clear(size_t i) {
    if (this->is_pointer[i]) {
        delete this->hash_table[i].second.pointer;
        this->hash_table[i].second.value = NO_VALUE;
        this->is_pointer[i] = false;
    }
}

void MinimizerIndex::clear() {
    for (size_t i = 0; i < this->hash_table.size(); i++) {
        this->clear(i);
    }    
}

//------------------------------------------------------------------------------

MinimizerIndex::minimizer_type
minimizer_unsafe(std::string::const_iterator begin, std::string::const_iterator end, size_t k) {
    MinimizerIndex::minimizer_type result(MinimizerIndex::NO_KEY, 0);

    MinimizerIndex::key_type key = 0;
    size_t valid_chars = 0;
    for (std::string::const_iterator iter = begin; iter != end; ++iter) {
        MinimizerIndex::key_type packed = MinimizerIndex::CHAR_TO_PACK[static_cast<unsigned char>(*iter)];
        if (packed > MinimizerIndex::PACK_MASK) {
            key = 0;
            valid_chars = 0;
            continue;
        }
        key = ((key << MinimizerIndex::PACK_WIDTH) | packed) & MinimizerIndex::KMER_MASK[k];
        valid_chars++;
        if (valid_chars >= k && key < result.first) {
            result.first = key;
            result.second = (iter - begin) + 1 - k;
        }
    }

    return result;
}

MinimizerIndex::minimizer_type
MinimizerIndex::minimizer(std::string::const_iterator begin, std::string::const_iterator end) const {
    if (end - begin < this->k()) {
        return minimizer_type(NO_KEY, 0);
    }
    return minimizer_unsafe(begin, end, this->k());
}

std::vector<MinimizerIndex::minimizer_type>
MinimizerIndex::minimizers(std::string::const_iterator begin, std::string::const_iterator end) const {
    std::vector<minimizer_type> result;
    size_t window_length = this->k() + this->w() - 1, total_length = end - begin;
    if (total_length < window_length) {
        return result;
    }

    for (size_t i = 0; i + window_length <= total_length; i++) {
        minimizer_type temp = minimizer_unsafe(begin + i, begin + i + window_length, this->k());
        if (temp.first != NO_KEY) {
            result.push_back(temp);
        }
    }

    return result;
}

void MinimizerIndex::insert(key_type key, pos_t pos) {
    if (key == NO_KEY) {
        return;
    }

    size_t offset = hash(key) & (this->capacity() - 1);
    for (size_t attempt = 0; attempt < this->capacity(); attempt++) {
        if (this->hash_table[offset].first == NO_KEY) {
            this->insert(key, encode(pos), offset);
            return;
        }
        if (this->hash_table[offset].first == key) {
            this->append(key, encode(pos), offset);
            return;
        }
        // Quadratic probing with triangular numbers.
        offset = (offset + attempt + 1) & (this->capacity() - 1);
    }

    // This should not happen.
    std::cerr << "error: [MinimizerIndex::insert()] Insertion failed for key " << key << std::endl;
}

void MinimizerIndex::insert(key_type key, code_type pos, size_t offset) {
    this->hash_table[offset].first = key;
    this->hash_table[offset].second.value = pos;
    this->header.keys++;
    this->header.values++;
    this->header.unique++;

    if (this->size() > this->max_keys()) {
        this->rehash();
    }
}

void MinimizerIndex::append(key_type key, code_type pos, size_t offset) {
    if (this->contains(offset, pos)) {
        return;
    }

    if (this->is_pointer[offset]) {
        std::vector<code_type>* occs = this->hash_table[offset].second.pointer;
        if (occs->size() + 1 > this->header.max_values) {
            this->header.values -= occs->size();
            this->header.frequent++;
            this->clear(offset);
        } else {
            occs->push_back(pos);
            std::sort(occs->begin(), occs->end());
            this->header.values++;
        }
    } else {
        this->header.unique--;
        if (this->header.max_values < 2) {
            this->hash_table[offset].second.value = NO_VALUE;
            this->header.values--;
            this->header.frequent++;
        } else {
            std::vector<code_type>* occs = new std::vector<code_type>(2);
            occs->at(0) = this->hash_table[offset].second.value;
            occs->at(1) = pos;
            if (occs->at(0) > occs->at(1)) {
                std::swap(occs->at(0), occs->at(1));
            }
            this->hash_table[offset].second.pointer = occs;
            this->is_pointer[offset] = true;
            this->header.values++;
        }
    }
}

bool MinimizerIndex::contains(size_t offset, code_type pos) const {
    if (this->is_pointer[offset]) {
        const std::vector<code_type>* occs = this->hash_table[offset].second.pointer;
        return std::binary_search(occs->begin(), occs->end(), pos);
    } else {
        return (this->hash_table[offset].second.value == pos);
    }
}

void MinimizerIndex::rehash() {
    std::vector<cell_type> new_hash_table(2 * this->capacity(), empty_cell());
    std::vector<bool> new_is_pointer(2 * this->capacity(), false);

    for (size_t i = 0; i < this->capacity(); i++) {
        key_type key = this->hash_table[i].first;
        if (key == NO_KEY) {
            continue;
        }

        size_t offset = hash(key) & (new_hash_table.size() - 1);
        for (size_t attempt = 0; attempt < new_hash_table.size(); attempt++) {
            if (new_hash_table[offset].first == NO_KEY) {
                new_hash_table[offset] = this->hash_table[i];
                new_is_pointer[offset] = this->is_pointer[i];
                break;
            }
            // Quadratic probing with triangular numbers.
            offset = (offset + attempt + 1) & (this->capacity() - 1);
        }

        // This should not happen.
        std::cerr << "error: [MinimizerIndex::rehash()] Insertion failed for key " << key << std::endl;
    }

    std::swap(this->hash_table, new_hash_table);
    std::swap(this->is_pointer, new_is_pointer);
    this->header.capacity = this->hash_table.size();
    this->header.max_keys = this->capacity() * MAX_LOAD_FACTOR;
}

//------------------------------------------------------------------------------

} // namespace vg