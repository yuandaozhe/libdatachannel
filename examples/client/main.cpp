/*
 * libdatachannel client example
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
 * Copyright (c) 2019 Murat Dogan
 * Copyright (c) 2020 Will Munn
 * Copyright (c) 2020 Nico Chatzi
 * Copyright (c) 2020 Lara Mackey
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include "rtc/rtc.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include <unordered_map>

using namespace rtc;
using namespace std;
using namespace chrono_literals;

using chrono::duration_cast;
using chrono::milliseconds;
using chrono::steady_clock;

using json = nlohmann::json;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

unordered_map<string, shared_ptr<PeerConnection>> peerConnectionMap;
unordered_map<string, shared_ptr<DataChannel>> dataChannelMap;

string localId;

shared_ptr<PeerConnection> createPeerConnection(const Configuration &config,
                                                weak_ptr<WebSocket> wws, string id);
string randomId(size_t length);

const size_t messageSize = 65535;
binary messageData(messageSize);

atomic<size_t> receivedSize = 0;

int main(int argc, char **argv) {
	if (argc < 2 || argc > 3) {
		cout << "Usage: " << (argc == 1 ? argv[0] : "client")
		     << " signaling_host [remote_id if offerer]" << endl;
		return -1;
	}

	fill(messageData.begin(), messageData.end(), byte(0xFF));

	rtc::InitLogger(LogLevel::Debug);

	Configuration config;
	config.iceServers.emplace_back("stun:stun.l.google.com:19302"); // change to your STUN server

	localId = randomId(4);
	cout << "The local ID is: " << localId << endl;

	auto ws = make_shared<WebSocket>();

	ws->onOpen([]() { cout << "WebSocket connected, signaling ready" << endl; });

	ws->onClosed([]() { cout << "WebSocket closed" << endl; });

	ws->onError([](const string &error) { cout << "WebSocket failed: " << error << endl; });

	ws->onMessage([&](const variant<binary, string> &data) {
		if (!holds_alternative<string>(data))
			return;

		json message = json::parse(get<string>(data));

		auto it = message.find("id");
		if (it == message.end())
			return;
		string id = it->get<string>();

		it = message.find("type");
		if (it == message.end())
			return;
		string type = it->get<string>();

		shared_ptr<PeerConnection> pc;
		if (auto jt = peerConnectionMap.find(id); jt != peerConnectionMap.end()) {
			pc = jt->second;
		} else if (type == "offer") {
			cout << "Answering to " + id << endl;
			pc = createPeerConnection(config, ws, id);
		} else {
			return;
		}

		if (type == "offer" || type == "answer") {
			auto sdp = message["description"].get<string>();
			pc->setRemoteDescription(Description(sdp, type));
		} else if (type == "candidate") {
			auto sdp = message["candidate"].get<string>();
			auto mid = message["mid"].get<string>();
			pc->addRemoteCandidate(Candidate(sdp, mid));
		}
	});

	const string url = "ws://" + string(argv[1]) + "/" + localId;
	ws->open(url);

	cout << "Waiting for signaling to be connected..." << endl;
	while (!ws->isOpen()) {
		if (ws->isClosed())
			return 1;
		this_thread::sleep_for(100ms);
	}

	if (argc == 3) {
		const string id = argv[2];

		cout << "Offering to " + id << endl;
		auto pc = createPeerConnection(config, ws, id);

		// We are the offerer, so create a data channel to initiate the process
		const string label = "benchmark";
		cout << "Creating DataChannel with label \"" << label << "\"" << endl;
		auto dc = pc->createDataChannel(label);

		dc->onOpen([id, wdc = make_weak_ptr(dc)]() {
			auto dc = wdc.lock();
			if (!dc)
				return;

			cout << "DataChannel from " << id << " open, sending data..." << endl;
			while (dc->bufferedAmount() == 0) {
				dc->send(messageData);
			}
		});

		dc->onBufferedAmountLow([wdc = make_weak_ptr(dc)]() {
			auto dc = wdc.lock();
			if (!dc)
				return;

			while (dc->bufferedAmount() == 0) {
				dc->send(messageData);
			}
		});

		dc->onClosed([id]() { cout << "DataChannel from " << id << " closed" << endl; });

		dc->onMessage([id](const variant<binary, string> &message) {
			// ignore
		});

		dataChannelMap.emplace(id, dc);
	}

	while (true) {
		const auto duration = 5s;
		const size_t receivedBase = receivedSize.load();
		const auto timeBase = steady_clock::now();

		this_thread::sleep_for(duration);

		const size_t delta = receivedSize.load() - receivedBase;
		const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - timeBase);

		size_t goodput = elapsed.count() > 0 ? delta / elapsed.count() : 0;
		cout << "Goodput: " << goodput * 0.001 << " MB/s"
		     << " (" << goodput * 0.001 * 8 << " Mbit/s)" << endl;
	}

	cout << "Cleaning up..." << endl;

	dataChannelMap.clear();
	peerConnectionMap.clear();
	return 0;
}

// Create and setup a PeerConnection
shared_ptr<PeerConnection> createPeerConnection(const Configuration &config,
                                                weak_ptr<WebSocket> wws, string id) {
	auto pc = make_shared<PeerConnection>(config);

	pc->onStateChange([](PeerConnection::State state) { cout << "State: " << state << endl; });

	pc->onGatheringStateChange(
	    [](PeerConnection::GatheringState state) { cout << "Gathering State: " << state << endl; });

	pc->onLocalDescription([wws, id](const Description &description) {
		json message = {
		    {"id", id}, {"type", description.typeString()}, {"description", string(description)}};

		if (auto ws = wws.lock())
			ws->send(message.dump());
	});

	pc->onLocalCandidate([wws, id](const Candidate &candidate) {
		json message = {{"id", id},
		                {"type", "candidate"},
		                {"candidate", string(candidate)},
		                {"mid", candidate.mid()}};

		if (auto ws = wws.lock())
			ws->send(message.dump());
	});

	pc->onDataChannel([id](shared_ptr<DataChannel> dc) {
		cout << "DataChannel from " << id << " received with label \"" << dc->label() << "\""
		     << endl;

		dc->onMessage([id](const variant<binary, string> &message) {
			if (holds_alternative<binary>(message)) {
				const auto &bin = get<binary>(message);
				receivedSize += bin.size();
			}
		});

		dc->onClosed([id]() { cout << "DataChannel from " << id << " closed" << endl; });

		dataChannelMap.emplace(id, dc);
	});

	peerConnectionMap.emplace(id, pc);
	return pc;
};

// Helper function to generate a random ID
string randomId(size_t length) {
	static const string characters(
	    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
	string id(length, '0');
	default_random_engine rng(random_device{}());
	uniform_int_distribution<int> dist(0, characters.size() - 1);
	generate(id.begin(), id.end(), [&]() { return characters.at(dist(rng)); });
	return id;
}
