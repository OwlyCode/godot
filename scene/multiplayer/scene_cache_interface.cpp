/*************************************************************************/
/*  scene_cache_interface.cpp                                            */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2022 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2022 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#include "scene_cache_interface.h"

#include "core/io/marshalls.h"
#include "scene/main/node.h"
#include "scene/main/window.h"

MultiplayerCacheInterface *SceneCacheInterface::_create(MultiplayerAPI *p_multiplayer) {
	return memnew(SceneCacheInterface(p_multiplayer));
}

void SceneCacheInterface::make_default() {
	MultiplayerAPI::create_default_cache_interface = _create;
}

void SceneCacheInterface::on_peer_change(int p_id, bool p_connected) {
	if (p_connected) {
		path_get_cache.insert(p_id, PathGetCache());
	} else {
		// Cleanup get cache.
		path_get_cache.erase(p_id);
		// Cleanup sent cache.
		// Some refactoring is needed to make this faster and do paths GC.
		List<NodePath> keys;
		path_send_cache.get_key_list(&keys);
		for (const NodePath &E : keys) {
			PathSentCache *psc = path_send_cache.getptr(E);
			psc->confirmed_peers.erase(p_id);
		}
	}
}

void SceneCacheInterface::process_simplify_path(int p_from, const uint8_t *p_packet, int p_packet_len) {
	Node *root_node = SceneTree::get_singleton()->get_root()->get_node(multiplayer->get_root_path());
	ERR_FAIL_COND(!root_node);
	ERR_FAIL_COND_MSG(p_packet_len < 38, "Invalid packet received. Size too small.");
	int ofs = 1;

	String methods_md5;
	methods_md5.parse_utf8((const char *)(p_packet + ofs), 32);
	ofs += 33;

	int id = decode_uint32(&p_packet[ofs]);
	ofs += 4;

	String paths;
	paths.parse_utf8((const char *)(p_packet + ofs), p_packet_len - ofs);

	NodePath path = paths;

	if (!path_get_cache.has(p_from)) {
		path_get_cache[p_from] = PathGetCache();
	}

	Node *node = root_node->get_node(path);
	ERR_FAIL_COND(node == nullptr);
	const bool valid_rpc_checksum = multiplayer->get_rpc_md5(node) == methods_md5;
	if (valid_rpc_checksum == false) {
		ERR_PRINT("The rpc node checksum failed. Make sure to have the same methods on both nodes. Node path: " + path);
	}

	PathGetCache::NodeInfo ni;
	ni.path = path;

	path_get_cache[p_from].nodes[id] = ni;

	// Encode path to send ack.
	CharString pname = String(path).utf8();
	int len = encode_cstring(pname.get_data(), nullptr);

	Vector<uint8_t> packet;

	packet.resize(1 + 1 + len);
	packet.write[0] = MultiplayerAPI::NETWORK_COMMAND_CONFIRM_PATH;
	packet.write[1] = valid_rpc_checksum;
	encode_cstring(pname.get_data(), &packet.write[2]);

	Ref<MultiplayerPeer> multiplayer_peer = multiplayer->get_multiplayer_peer();
	ERR_FAIL_COND(multiplayer_peer.is_null());

	multiplayer_peer->set_transfer_channel(0);
	multiplayer_peer->set_transfer_mode(Multiplayer::TRANSFER_MODE_RELIABLE);
	multiplayer_peer->set_target_peer(p_from);
	multiplayer_peer->put_packet(packet.ptr(), packet.size());
}

void SceneCacheInterface::process_confirm_path(int p_from, const uint8_t *p_packet, int p_packet_len) {
	ERR_FAIL_COND_MSG(p_packet_len < 3, "Invalid packet received. Size too small.");

	const bool valid_rpc_checksum = p_packet[1];

	String paths;
	paths.parse_utf8((const char *)&p_packet[2], p_packet_len - 2);

	NodePath path = paths;

	if (valid_rpc_checksum == false) {
		ERR_PRINT("The rpc node checksum failed. Make sure to have the same methods on both nodes. Node path: " + path);
	}

	PathSentCache *psc = path_send_cache.getptr(path);
	ERR_FAIL_COND_MSG(!psc, "Invalid packet received. Tries to confirm a path which was not found in cache.");

	Map<int, bool>::Element *E = psc->confirmed_peers.find(p_from);
	ERR_FAIL_COND_MSG(!E, "Invalid packet received. Source peer was not found in cache for the given path.");
	E->get() = true;
}

bool SceneCacheInterface::_send_confirm_path(Node *p_node, NodePath p_path, PathSentCache *psc, int p_target) {
	bool has_all_peers = true;
	List<int> peers_to_add; // If one is missing, take note to add it.

	for (const Set<int>::Element *E = multiplayer->get_connected_peers().front(); E; E = E->next()) {
		if (p_target < 0 && E->get() == -p_target) {
			continue; // Continue, excluded.
		}

		if (p_target > 0 && E->get() != p_target) {
			continue; // Continue, not for this peer.
		}

		Map<int, bool>::Element *F = psc->confirmed_peers.find(E->get());

		if (!F || !F->get()) {
			// Path was not cached, or was cached but is unconfirmed.
			if (!F) {
				// Not cached at all, take note.
				peers_to_add.push_back(E->get());
			}

			has_all_peers = false;
		}
	}

	if (peers_to_add.size() > 0) {
		// Those that need to be added, send a message for this.

		// Encode function name.
		const CharString path = String(p_path).utf8();
		const int path_len = encode_cstring(path.get_data(), nullptr);

		// Extract MD5 from rpc methods list.
		const String methods_md5 = multiplayer->get_rpc_md5(p_node);
		const int methods_md5_len = 33; // 32 + 1 for the `0` that is added by the encoder.

		Vector<uint8_t> packet;
		packet.resize(1 + 4 + path_len + methods_md5_len);
		int ofs = 0;

		packet.write[ofs] = MultiplayerAPI::NETWORK_COMMAND_SIMPLIFY_PATH;
		ofs += 1;

		ofs += encode_cstring(methods_md5.utf8().get_data(), &packet.write[ofs]);

		ofs += encode_uint32(psc->id, &packet.write[ofs]);

		ofs += encode_cstring(path.get_data(), &packet.write[ofs]);

		Ref<MultiplayerPeer> multiplayer_peer = multiplayer->get_multiplayer_peer();
		ERR_FAIL_COND_V(multiplayer_peer.is_null(), false);

		for (int &E : peers_to_add) {
			multiplayer_peer->set_target_peer(E); // To all of you.
			multiplayer_peer->set_transfer_channel(0);
			multiplayer_peer->set_transfer_mode(Multiplayer::TRANSFER_MODE_RELIABLE);
			multiplayer_peer->put_packet(packet.ptr(), packet.size());

			psc->confirmed_peers.insert(E, false); // Insert into confirmed, but as false since it was not confirmed.
		}
	}

	return has_all_peers;
}

bool SceneCacheInterface::is_cache_confirmed(NodePath p_path, int p_peer) {
	const PathSentCache *psc = path_send_cache.getptr(p_path);
	ERR_FAIL_COND_V(!psc, false);
	const Map<int, bool>::Element *F = psc->confirmed_peers.find(p_peer);
	ERR_FAIL_COND_V(!F, false); // Should never happen.
	return F->get();
}

bool SceneCacheInterface::send_object_cache(Object *p_obj, NodePath p_path, int p_peer_id, int &r_id) {
	Node *node = Object::cast_to<Node>(p_obj);
	ERR_FAIL_COND_V(!node, false);
	// See if the path is cached.
	PathSentCache *psc = path_send_cache.getptr(p_path);
	if (!psc) {
		// Path is not cached, create.
		path_send_cache[p_path] = PathSentCache();
		psc = path_send_cache.getptr(p_path);
		psc->id = last_send_cache_id++;
	}
	r_id = psc->id;

	return _send_confirm_path(node, p_path, psc, p_peer_id);
}

Object *SceneCacheInterface::get_cached_object(int p_from, uint32_t p_cache_id) {
	Node *root_node = SceneTree::get_singleton()->get_root()->get_node(multiplayer->get_root_path());
	ERR_FAIL_COND_V(!root_node, nullptr);
	Map<int, PathGetCache>::Element *E = path_get_cache.find(p_from);
	ERR_FAIL_COND_V_MSG(!E, nullptr, vformat("No cache found for peer %d.", p_from));

	Map<int, PathGetCache::NodeInfo>::Element *F = E->get().nodes.find(p_cache_id);
	ERR_FAIL_COND_V_MSG(!F, nullptr, vformat("ID %d not found in cache of peer %d.", p_cache_id, p_from));

	PathGetCache::NodeInfo *ni = &F->get();
	Node *node = root_node->get_node(ni->path);
	if (!node) {
		ERR_PRINT("Failed to get cached path: " + String(ni->path) + ".");
	}
	return node;
}

void SceneCacheInterface::clear() {
	path_get_cache.clear();
	path_send_cache.clear();
	last_send_cache_id = 1;
}
