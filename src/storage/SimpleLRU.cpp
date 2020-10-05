#include "SimpleLRU.h"

namespace Afina {
namespace Backend {

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::Put(const std::string &key, const std::string &value) {
    if (key.size() + value.size() > _max_size) {
        return false;
    }

    auto it = _lru_index.find(key);
    if (it == _lru_index.end()) {
        return _push(key, value);
    }

    auto& search = it->second.get();
    _push(search);

    while (_lru_head && _curr_size + value.size() - search.value.size() > _max_size) {
        _pop(*_lru_head.get());
    }

    search.value = value;
    _curr_size += value.size() - search.value.size();

    return true;
}

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::PutIfAbsent(const std::string &key, const std::string &value) {
    if (key.size() + value.size() > _max_size) {
        return false;
    }

    auto it = _lru_index.find(key);
    if (it == _lru_index.end()) {
        return _push(key, value);
    }

    return false;
}

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::Set(const std::string &key, const std::string &value) {
    if (key.size() + value.size() > _max_size) {
        return false;
    }

    auto it = _lru_index.find(key);
    if (it == _lru_index.end()) {
        return false;
    }

    auto& search = it->second.get();
    _push(search);

    while (_lru_head && _curr_size + value.size() - search.value.size() > _max_size) {
        _pop(*_lru_head.get());
    }

    search.value = value;
    _curr_size += value.size() - search.value.size();

    return true;
}

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::Delete(const std::string &key) {
    auto it = _lru_index.find(key);
    if (it == _lru_index.end()) {
        return false;
    }

    auto& search = it->second.get();
    _pop(search);

    return true;
}

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::Get(const std::string &key, std::string &value) {
    auto it = _lru_index.find(key);
    if (it == _lru_index.end()) {
        return false;
    }

    auto& search = it->second.get();
    value = search.value;

    _push(search);

    return true;
}

bool SimpleLRU::_push(const std::string &key, const std::string &value) {
    std::size_t node_size = key.size() + value.size();

    while (_lru_head && _curr_size + node_size > _max_size) {
        _pop(*_lru_head.get());
    }

    auto new_node = new lru_node{key, value, nullptr, nullptr};
    if (!new_node) {
        throw std::bad_alloc();
    }

    if (!_lru_tail) {
        _lru_head.reset(new_node);
        _lru_tail = new_node;
    }
    else {
        _lru_tail->next.reset(new_node);
        new_node->prev = _lru_tail;
        _lru_tail = new_node;
    }

    _lru_index.insert({new_node->key, *new_node});
    _curr_size += node_size;

    return true;
}

bool SimpleLRU::_push(lru_node& node) {
    if (node.next) {
        node.next->prev = node.prev;
    }
    else {
        return true;
    }

    if (node.prev) {
        node.prev->next.release();
        node.prev->next = std::move(node.next);
    }
    else {
        _lru_head.release();
        _lru_head = std::move(node.next);
    }

    _lru_tail->next.reset(&node);
    node.prev = _lru_tail;
    node.next.release();
    node.next.reset();
    _lru_tail = &node;

    return true;
}

bool SimpleLRU::_pop(lru_node& node) {
    _lru_index.erase(node.key);
    _curr_size -= node.key.size() + node.value.size();

    if (node.next) {
        node.next->prev = node.prev;
    }
    else {
        _lru_tail = node.prev;
    }

    if (node.prev) {
        if (node.next) {
            node.prev->next = std::move(node.next);
        }
        else {
            node.prev->next.reset();
        }
    }
    else {
        if (node.next) {
            _lru_head = std::move(node.next);
        }
        else {
            _lru_head.reset();
        }
    }
    
    return true;
}

// void SimpleLRU::_print_list() {
//     auto curr = _lru_head.get();
//     while (curr) {
//         std::cout << curr->prev << ' ' << curr << ' ' << curr->next << std::endl;
//         std::cout << curr->key << ' ' << curr->value << std::endl;
//         curr = curr->next.get();
//     }
// }

} // namespace Backend
} // namespace Afina
