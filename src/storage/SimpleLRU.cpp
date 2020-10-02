#include "SimpleLRU.h"

namespace Afina {
namespace Backend {

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::Put(const std::string &key, const std::string &value) {
    auto it = _lru_index.find(key);
    if (it != _lru_index.end()) {
        pop(it->second.get());
    }

    if (!push(key, value)) {
        return false;
    }

    return true;
}

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::PutIfAbsent(const std::string &key, const std::string &value) {
    auto it = _lru_index.find(key);
    if (it == _lru_index.end()) {
        if (!push(key, value)) {
            return false;
        }
    }
    else {
        return false;
    }

    return true;
}

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::Set(const std::string &key, const std::string &value) {
    auto it = _lru_index.find(key);
    if (it == _lru_index.end()) {
        return false;
    }

    auto& search = it->second.get();
    pop(search);
    if (!push(key, value)) {
        push(key, search.value);
        return false;
    }

    return true;
}

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::Delete(const std::string &key) {
    auto it = _lru_index.find(key);
    if (it == _lru_index.end()) {
        return false;
    }

    auto& search = it->second.get();
    pop(search);

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

    pop(search);
    if (!push(key, value)) {
        return false;
    }

    return true;
}

bool SimpleLRU::push(const std::string &key, const std::string &value) {
    if (key.size() + value.size() > _max_size) {
        return false;
    }

    while (_lru_head && _curr_size + key.size() + value.size() > _max_size) {
        auto k = _lru_head->key;
        auto val = _lru_head->value;
        pop(*_lru_head.get());
    }

    auto* new_node = new lru_node{key, value, nullptr, nullptr};
    if (!new_node) {
        return false;
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

    _lru_index.insert({key, std::ref(*new_node)});
    _curr_size += key.size() + value.size();

    return true;
}

bool SimpleLRU::pop(lru_node& node) {
    _lru_index.erase(node.key);
    _curr_size -= node.key.size() + node.value.size();

    if (node.next) {
        if (node.prev) {
            node.next->prev = node.prev;
        }
        else {
            node.next->prev = nullptr;
        }
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

} // namespace Backend
} // namespace Afina
