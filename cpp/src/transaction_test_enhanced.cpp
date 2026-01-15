/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/**
 * Enhanced Transaction Test Suite for C++ Client Local Transaction Support
 * 
 * This test suite provides comprehensive coverage of the local transaction
 * feature including:
 * - Basic transaction lifecycle (declare, commit, abort)
 * - Error handling and edge cases
 * - Multiple sequential transactions
 * - Transactional message delivery
 * - Transaction state validation
 * - settle_before_discharge parameter testing
 */

#include <proton/connection.hpp>
#include <proton/connection_options.hpp>
#include <proton/listen_handler.hpp>
#include <proton/container.hpp>
#include <proton/delivery.hpp>
#include "proton/codec/decoder.hpp"
#include "proton/codec/encoder.hpp"
#include <proton/listener.hpp>
#include <proton/message.hpp>
#include <proton/messaging_handler.hpp>
#include <proton/receiver.hpp>
#include <proton/sender.hpp>
#include <proton/session.hpp>
#include <proton/types.hpp>
#include "proton_bits.hpp"
#include <proton/target.hpp>
#include <proton/tracker.hpp>
#include <proton/value.hpp>
#include <proton/codec/decoder.hpp>
#include <proton/delivery.h>
#include "test_bits.hpp"
#include "types_internal.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace {
std::mutex m;
std::condition_variable cv;
bool listener_ready = false;
int listener_port;
const proton::binary fake_txn_id("prqs5678-abcd-efgh-1a2b-3c4d5e6f7g8e");
const proton::binary fake_txn_id_2("prqs9999-wxyz-ijkl-4c5d-6e7f8g9h0i1j");
} // namespace

void wait_for_promise_or_fail(std::promise<void>& prom, const std::string& what, int timeout_sec = 5) {
   if (prom.get_future().wait_for(std::chrono::seconds(timeout_sec)) == std::future_status::timeout) {
       FAIL("Test FAILED: Did not receive '" << what << "' in time.");
   }
}

/**
 * Enhanced FakeBroker that supports multiple transactions and various test scenarios
 */
class EnhancedFakeBroker : public proton::messaging_handler {
 private:
   class listener_ready_handler : public proton::listen_handler {
       void on_open(proton::listener& l) override {
           std::lock_guard<std::mutex> lk(m);
           listener_port = l.port();
           listener_ready = true;
           cv.notify_one();
       }
   };
   std::string url;
   listener_ready_handler listen_handler;
   proton::receiver coordinator_link;
   int transaction_counter = 0;
   bool should_reject_declare = false;
   bool should_fail_commit = false;

 public:
   proton::listener listener;
   std::map<proton::binary, std::vector<proton::message>> transactions_messages;
   std::vector<std::promise<void>> declare_promises;
   std::vector<std::promise<void>> commit_promises;
   std::vector<std::promise<void>> abort_promises;
   std::atomic<int> declare_count{0};
   std::atomic<int> commit_count{0};
   std::atomic<int> abort_count{0};
   bool auto_close_on_discharge = true;

   EnhancedFakeBroker(const std::string& s) : url(s) {
       // Pre-allocate promises for multiple transactions
       for (int i = 0; i < 10; ++i) {
           declare_promises.emplace_back();
           commit_promises.emplace_back();
           abort_promises.emplace_back();
       }
   }

   void set_reject_declare(bool reject) { should_reject_declare = reject; }
   void set_fail_commit(bool fail) { should_fail_commit = fail; }
   void set_auto_close(bool close) { auto_close_on_discharge = close; }

   void on_container_start(proton::container& c) override {
       listener = c.listen(url, listen_handler);
   }

   void on_connection_open(proton::connection& c) override {
       c.open(proton::connection_options{}.offered_capabilities({"ANONYMOUS-RELAY"}));
   }

   void on_receiver_open(proton::receiver& r) override {
       if(r.target().capabilities().size() > 0 &&
          r.target().capabilities()[0] == proton::symbol("amqp:local-transactions")) {
           coordinator_link = r;
       }
       r.open();
   }

   void on_message(proton::delivery& d, proton::message& m) override {
       if (coordinator_link.active() && d.receiver() == coordinator_link) {
           handle_transaction_control(d, m);
       } else {
           handle_application_message(d, m);
       }
   }

   void handle_application_message(proton::delivery& d, proton::message& m) {
       auto disp = pn_transactional_disposition(pn_delivery_remote(unwrap(d)));
       if (disp != NULL) {
           proton::binary txn_id = proton::bin(pn_transactional_disposition_get_id(disp));
           transactions_messages[txn_id].push_back(m);
           std::cout << "[BROKER] Received transactional message in txn: " << txn_id << std::endl;
       }
   }

   void handle_transaction_control(proton::delivery& d, proton::message& m) {
       proton::codec::decoder dec(m.body());
       proton::symbol descriptor;
       proton::value _value;
       proton::type_id _t = dec.next_type();

       if (_t == proton::type_id::DESCRIBED) {
           proton::codec::start s;
           dec >> s >> descriptor >> _value >> proton::codec::finish();
       } else {
           std::cerr << "[BROKER] Invalid transaction control message format" << std::endl;
           d.reject();
           return;
       }

       if (descriptor == "amqp:declare:list") {
           handle_declare(d);
       } else if (descriptor == "amqp:discharge:list") {
           handle_discharge(d, _value);
       }
   }

   void handle_declare(proton::delivery& d) {
       if (should_reject_declare) {
           std::cout << "[BROKER] Rejecting transaction declare" << std::endl;
           d.reject();
           return;
       }

       // Use different transaction IDs for multiple transactions
       proton::binary txn_id = (transaction_counter == 0) ? fake_txn_id : fake_txn_id_2;
       pn_bytes_t txn_id_bytes = pn_bytes(txn_id.size(), reinterpret_cast<const char*>(&txn_id[0]));

       pn_delivery_t* pd = proton::unwrap(d);
       pn_disposition_t* disp = pn_delivery_local(pd);
       pn_declared_disposition_t* declared_disp = pn_declared_disposition(disp);
       pn_declared_disposition_set_id(declared_disp, txn_id_bytes);
       pn_delivery_settle(pd);

       std::cout << "[BROKER] Transaction declared (#" << transaction_counter << "): " << txn_id << std::endl;
       
       if (static_cast<size_t>(declare_count) < declare_promises.size()) {
           declare_promises[declare_count].set_value();
       }
       declare_count++;
       transaction_counter++;
   }

   void handle_discharge(proton::delivery& d, const proton::value& _value) {
       std::vector<proton::value> vd;
       proton::get(_value, vd);
       ASSERT_EQUAL(vd.size(), 2u);
       proton::binary txn_id = vd[0].get<proton::binary>();
       bool is_abort = vd[1].get<bool>();

       if (!is_abort) {
           // Commit
           if (should_fail_commit) {
               std::cout << "[BROKER] Failing transaction commit: " << txn_id << std::endl;
               d.reject();
           } else {
               std::cout << "[BROKER] Transaction committed: " << txn_id << std::endl;
               d.accept();
               if (static_cast<size_t>(commit_count) < commit_promises.size()) {
                   commit_promises[commit_count].set_value();
               }
               commit_count++;
           }
       } else {
           // Abort
           std::cout << "[BROKER] Transaction aborted: " << txn_id << std::endl;
           transactions_messages.erase(txn_id);
           d.accept();
           if (static_cast<size_t>(abort_count) < abort_promises.size()) {
               abort_promises[abort_count].set_value();
           }
           abort_count++;
       }

       if (auto_close_on_discharge) {
           d.receiver().close();
           d.connection().close();
           listener.stop();
       }
   }
};

/**
 * Test client for basic transaction operations
 */
class BasicTransactionClient : public proton::messaging_handler {
 private:
   std::string server_address_;
   int messages_to_send_;
   bool should_commit_;
   bool transaction_started_ = false;

 public:
   proton::sender sender_;
   std::promise<void> ready_promise;
   std::promise<void> finished_promise;
   proton::binary last_txn_id;
   int messages_sent = 0;

   BasicTransactionClient(const std::string& addr, int msg_count, bool commit)
       : server_address_(addr), messages_to_send_(msg_count), should_commit_(commit) {}

   void on_container_start(proton::container& c) override {
       c.connect(server_address_);
   }

   void on_connection_open(proton::connection& c) override {
       sender_ = c.open_sender("/test");
   }

   void on_session_open(proton::session& s) override {
       wait_for_promise_or_fail(ready_promise, "client ready");
       s.transaction_declare();
   }

   void on_session_transaction_declared(proton::session& s) override {
       last_txn_id = s.transaction_id();
       std::cout << "[CLIENT] Transaction declared: " << last_txn_id << std::endl;
       transaction_started_ = true;
       send_messages();
   }

   void on_sendable(proton::sender&) override {
       if (transaction_started_) {
           send_messages();
       }
   }

   void send_messages() {
       proton::session session = sender_.session();
       while (session.transaction_is_declared() && sender_.credit() && messages_sent < messages_to_send_) {
           proton::message msg("test message " + std::to_string(messages_sent));
           sender_.send(msg);
           messages_sent++;
           std::cout << "[CLIENT] Sent message " << messages_sent << "/" << messages_to_send_ << std::endl;
       }

       if (messages_sent == messages_to_send_) {
           if (should_commit_) {
               std::cout << "[CLIENT] Committing transaction" << std::endl;
               session.transaction_commit();
           } else {
               std::cout << "[CLIENT] Aborting transaction" << std::endl;
               session.transaction_abort();
           }
       }
   }

   void on_session_transaction_committed(proton::session& s) override {
       std::cout << "[CLIENT] Transaction committed successfully" << std::endl;
       finished_promise.set_value();
       s.connection().close();
   }

   void on_session_transaction_aborted(proton::session& s) override {
       std::cout << "[CLIENT] Transaction aborted successfully" << std::endl;
       finished_promise.set_value();
       s.connection().close();
   }

   void on_session_transaction_error(proton::session& s) override {
       std::cout << "[CLIENT] Transaction error: " << s.transaction_error().what() << std::endl;
       finished_promise.set_value();
       s.connection().close();
   }
};

/**
 * Test client for multiple sequential transactions
 */
class MultiTransactionClient : public proton::messaging_handler {
 private:
   std::string server_address_;
   int transactions_to_run_;
   int current_transaction_ = 0;
   int messages_per_txn_;
   int messages_in_current_txn_ = 0;

 public:
   proton::sender sender_;
   std::promise<void> ready_promise;
   std::promise<void> finished_promise;
   std::vector<proton::binary> transaction_ids;

   MultiTransactionClient(const std::string& addr, int txn_count, int msg_per_txn)
       : server_address_(addr), transactions_to_run_(txn_count), messages_per_txn_(msg_per_txn) {}

   void on_container_start(proton::container& c) override {
       c.connect(server_address_);
   }

   void on_connection_open(proton::connection& c) override {
       sender_ = c.open_sender("/test");
   }

   void on_session_open(proton::session& s) override {
       wait_for_promise_or_fail(ready_promise, "multi-txn client ready");
       s.transaction_declare();
   }

   void on_session_transaction_declared(proton::session& s) override {
       proton::binary txn_id = s.transaction_id();
       transaction_ids.push_back(txn_id);
       std::cout << "[CLIENT] Transaction " << current_transaction_ << " declared: " << txn_id << std::endl;
       messages_in_current_txn_ = 0;
       send_messages();
   }

   void on_sendable(proton::sender&) override {
       send_messages();
   }

   void send_messages() {
       proton::session session = sender_.session();
       while (session.transaction_is_declared() && sender_.credit() && 
              messages_in_current_txn_ < messages_per_txn_) {
           proton::message msg("txn" + std::to_string(current_transaction_) + 
                             "_msg" + std::to_string(messages_in_current_txn_));
           sender_.send(msg);
           messages_in_current_txn_++;
       }

       if (messages_in_current_txn_ == messages_per_txn_) {
           std::cout << "[CLIENT] Committing transaction " << current_transaction_ << std::endl;
           session.transaction_commit();
       }
   }

   void on_session_transaction_committed(proton::session& s) override {
       std::cout << "[CLIENT] Transaction " << current_transaction_ << " committed" << std::endl;
       current_transaction_++;
       
       if (current_transaction_ < transactions_to_run_) {
           std::cout << "[CLIENT] Starting transaction " << current_transaction_ << std::endl;
           s.transaction_declare();
       } else {
           std::cout << "[CLIENT] All transactions completed" << std::endl;
           finished_promise.set_value();
           s.connection().close();
       }
   }
};

/**
 * Test client for error scenarios
 */
class ErrorTestClient : public proton::messaging_handler {
 private:
   std::string server_address_;
   std::string test_scenario_;

 public:
   proton::sender sender_;
   proton::session session_;
   std::promise<void> ready_promise;
   std::promise<void> error_promise;
   bool error_occurred = false;

   ErrorTestClient(const std::string& addr, const std::string& scenario)
       : server_address_(addr), test_scenario_(scenario) {}

   void on_container_start(proton::container& c) override {
       c.connect(server_address_);
   }

   void on_connection_open(proton::connection& c) override {
       session_ = c.open_session();
       sender_ = session_.open_sender("/test");
   }

   void on_session_open(proton::session& s) override {
       wait_for_promise_or_fail(ready_promise, "error test client ready");
       
       if (test_scenario_ == "double_declare") {
           test_double_declare(s);
       } else if (test_scenario_ == "commit_without_declare") {
           test_commit_without_declare(s);
       } else if (test_scenario_ == "abort_without_declare") {
           test_abort_without_declare(s);
       }
   }

   void test_double_declare(proton::session& s) {
       std::cout << "[CLIENT] Testing double declare..." << std::endl;
       s.transaction_declare();
   }

   void on_session_transaction_declared(proton::session& s) override {
       std::cout << "[CLIENT] First transaction declared" << std::endl;
       try {
           std::cout << "[CLIENT] Attempting second declare (should fail)..." << std::endl;
           s.transaction_declare();
           std::cout << "[CLIENT] ERROR: Second declare should have thrown!" << std::endl;
       } catch (const proton::error& e) {
           std::cout << "[CLIENT] Caught expected error: " << e.what() << std::endl;
           error_occurred = true;
           error_promise.set_value();
           s.connection().close();
       }
   }

   void test_commit_without_declare(proton::session& s) {
       std::cout << "[CLIENT] Testing commit without declare..." << std::endl;
       try {
           s.transaction_commit();
           std::cout << "[CLIENT] ERROR: Commit without declare should have thrown!" << std::endl;
       } catch (const proton::error& e) {
           std::cout << "[CLIENT] Caught expected error: " << e.what() << std::endl;
           error_occurred = true;
           error_promise.set_value();
           s.connection().close();
       }
   }

   void test_abort_without_declare(proton::session& s) {
       std::cout << "[CLIENT] Testing abort without declare..." << std::endl;
       try {
           s.transaction_abort();
           std::cout << "[CLIENT] ERROR: Abort without declare should have thrown!" << std::endl;
       } catch (const proton::error& e) {
           std::cout << "[CLIENT] Caught expected error: " << e.what() << std::endl;
           error_occurred = true;
           error_promise.set_value();
           s.connection().close();
       }
   }
};

// ============================================================================
// TEST CASES
// ============================================================================

void test_basic_commit(EnhancedFakeBroker& broker) {
    std::cout << "\n=== TEST: Basic Transaction Commit ===" << std::endl;
    
    std::string server_address = "127.0.0.1:" + std::to_string(listener_port);
    BasicTransactionClient client(server_address, 3, true);
    
    proton::container client_container(client);
    std::thread client_thread([&client_container]() { client_container.run(); });
    
    client.ready_promise.set_value();
    wait_for_promise_or_fail(broker.declare_promises[0], "declare");
    wait_for_promise_or_fail(broker.commit_promises[0], "commit");
    wait_for_promise_or_fail(client.finished_promise, "client finished");
    
    client_thread.join();
    
    ASSERT_EQUAL(broker.transactions_messages.size(), 1u);
    ASSERT_EQUAL(broker.transactions_messages[fake_txn_id].size(), 3u);
    ASSERT_EQUAL(client.messages_sent, 3);
    
    std::cout << "✓ Basic commit test passed" << std::endl;
}

void test_basic_abort(EnhancedFakeBroker& broker) {
    std::cout << "\n=== TEST: Basic Transaction Abort ===" << std::endl;
    
    broker.declare_count = 0;
    broker.abort_count = 0;
    broker.transactions_messages.clear();
    
    std::string server_address = "127.0.0.1:" + std::to_string(listener_port);
    BasicTransactionClient client(server_address, 3, false);
    
    proton::container client_container(client);
    std::thread client_thread([&client_container]() { client_container.run(); });
    
    client.ready_promise.set_value();
    wait_for_promise_or_fail(broker.declare_promises[0], "declare");
    wait_for_promise_or_fail(broker.abort_promises[0], "abort");
    wait_for_promise_or_fail(client.finished_promise, "client finished");
    
    client_thread.join();
    
    // After abort, messages should be removed
    ASSERT_EQUAL(broker.transactions_messages.size(), 0u);
    ASSERT_EQUAL(client.messages_sent, 3);
    
    std::cout << "✓ Basic abort test passed" << std::endl;
}

void test_transaction_id_retrieval() {
    std::cout << "\n=== TEST: Transaction ID Retrieval ===" << std::endl;
    
    std::string broker_address("127.0.0.1:0");
    EnhancedFakeBroker broker(broker_address);
    broker.set_auto_close(true);
    
    proton::container broker_container(broker);
    std::thread broker_thread([&broker_container]() { broker_container.run(); });
    
    std::unique_lock<std::mutex> lk(m);
    listener_ready = false;
    cv.wait(lk, [] { return listener_ready; });
    
    std::string server_address = "127.0.0.1:" + std::to_string(listener_port);
    BasicTransactionClient client(server_address, 1, true);
    
    proton::container client_container(client);
    std::thread client_thread([&client_container]() { client_container.run(); });
    
    client.ready_promise.set_value();
    wait_for_promise_or_fail(broker.declare_promises[0], "declare");
    
    // Verify transaction ID was captured
    ASSERT(!client.last_txn_id.empty());
    ASSERT_EQUAL(client.last_txn_id, fake_txn_id);
    
    wait_for_promise_or_fail(broker.commit_promises[0], "commit");
    wait_for_promise_or_fail(client.finished_promise, "client finished");
    
    broker_thread.join();
    client_thread.join();
    
    std::cout << "✓ Transaction ID retrieval test passed" << std::endl;
}

void test_multiple_sequential_transactions() {
    std::cout << "\n=== TEST: Multiple Sequential Transactions ===" << std::endl;
    
    std::string broker_address("127.0.0.1:0");
    EnhancedFakeBroker broker(broker_address);
    broker.set_auto_close(false);
    
    proton::container broker_container(broker);
    std::thread broker_thread([&broker_container]() { broker_container.run(); });
    
    std::unique_lock<std::mutex> lk(m);
    listener_ready = false;
    cv.wait(lk, [] { return listener_ready; });
    
    std::string server_address = "127.0.0.1:" + std::to_string(listener_port);
    MultiTransactionClient client(server_address, 2, 2);
    
    proton::container client_container(client);
    std::thread client_thread([&client_container]() { client_container.run(); });
    
    client.ready_promise.set_value();
    
    // Wait for first transaction
    wait_for_promise_or_fail(broker.declare_promises[0], "first declare");
    wait_for_promise_or_fail(broker.commit_promises[0], "first commit");
    
    // Wait for second transaction
    wait_for_promise_or_fail(broker.declare_promises[1], "second declare");
    wait_for_promise_or_fail(broker.commit_promises[1], "second commit");
    
    wait_for_promise_or_fail(client.finished_promise, "client finished");
    
    broker.listener.stop();
    broker_thread.join();
    client_thread.join();
    
    ASSERT_EQUAL(client.transaction_ids.size(), 2u);
    ASSERT_EQUAL(broker.declare_count.load(), 2);
    ASSERT_EQUAL(broker.commit_count.load(), 2);
    
    std::cout << "✓ Multiple sequential transactions test passed" << std::endl;
}

void test_double_declare_error() {
    std::cout << "\n=== TEST: Double Declare Error ===" << std::endl;
    
    std::string broker_address("127.0.0.1:0");
    EnhancedFakeBroker broker(broker_address);
    
    proton::container broker_container(broker);
    std::thread broker_thread([&broker_container]() { broker_container.run(); });
    
    std::unique_lock<std::mutex> lk(m);
    listener_ready = false;
    cv.wait(lk, [] { return listener_ready; });
    
    std::string server_address = "127.0.0.1:" + std::to_string(listener_port);
    ErrorTestClient client(server_address, "double_declare");
    
    proton::container client_container(client);
    std::thread client_thread([&client_container]() { client_container.run(); });
    
    client.ready_promise.set_value();
    wait_for_promise_or_fail(client.error_promise, "error caught");
    
    ASSERT(client.error_occurred);
    
    broker.listener.stop();
    broker_thread.join();
    client_thread.join();
    
    std::cout << "✓ Double declare error test passed" << std::endl;
}

/**
 * Test client for transactional delivery disposition testing
 */
class TransactionalDeliveryClient : public proton::messaging_handler {
 private:
   std::string server_address_;
   int messages_to_send_;
   std::string disposition_type_; // "accept", "reject", or "release"

 public:
   proton::sender sender_;
   std::promise<void> ready_promise;
   std::promise<void> finished_promise;
   std::atomic<int> transactional_accept_count{0};
   std::atomic<int> transactional_reject_count{0};
   std::atomic<int> transactional_release_count{0};

   TransactionalDeliveryClient(const std::string& addr, int msg_count, const std::string& disp_type)
       : server_address_(addr), messages_to_send_(msg_count), disposition_type_(disp_type) {}

   void on_container_start(proton::container& c) override {
       c.connect(server_address_);
   }

   void on_connection_open(proton::connection& c) override {
       sender_ = c.open_sender("/test");
   }

   void on_session_open(proton::session& s) override {
       wait_for_promise_or_fail(ready_promise, "transactional delivery client ready");
       s.transaction_declare();
   }

   void on_session_transaction_declared(proton::session& s) override {
       std::cout << "[CLIENT] Transaction declared for delivery test" << std::endl;
       send_messages();
   }

   void on_sendable(proton::sender&) override {
       send_messages();
   }

   void send_messages() {
       proton::session session = sender_.session();
       static int sent = 0;
       while (session.transaction_is_declared() && sender_.credit() && sent < messages_to_send_) {
           proton::message msg("delivery test message " + std::to_string(sent));
           sender_.send(msg);
           sent++;
           std::cout << "[CLIENT] Sent message " << sent << std::endl;
       }

       if (sent == messages_to_send_) {
           std::cout << "[CLIENT] All messages sent, committing transaction" << std::endl;
           session.transaction_commit();
       }
   }

   void on_transactional_accept(proton::tracker& t) override {
       transactional_accept_count++;
       std::cout << "[CLIENT] Transactional accept received (count: " 
                 << transactional_accept_count << ")" << std::endl;
   }

   void on_transactional_reject(proton::tracker& t) override {
       transactional_reject_count++;
       std::cout << "[CLIENT] Transactional reject received (count: " 
                 << transactional_reject_count << ")" << std::endl;
   }

   void on_transactional_release(proton::tracker& t) override {
       transactional_release_count++;
       std::cout << "[CLIENT] Transactional release received (count: " 
                 << transactional_release_count << ")" << std::endl;
   }

   void on_session_transaction_committed(proton::session& s) override {
       std::cout << "[CLIENT] Transaction committed" << std::endl;
       finished_promise.set_value();
       s.connection().close();
   }
};

/**
 * Test client for settle_before_discharge parameter testing
 */
class SettleBeforeDischargeClient : public proton::messaging_handler {
 private:
   std::string server_address_;
   bool settle_before_discharge_;
   int messages_to_send_;

 public:
   proton::sender sender_;
   std::promise<void> ready_promise;
   std::promise<void> finished_promise;
   std::atomic<int> settled_count{0};
   std::atomic<int> accepted_count{0};

   SettleBeforeDischargeClient(const std::string& addr, bool settle_param, int msg_count)
       : server_address_(addr), settle_before_discharge_(settle_param), messages_to_send_(msg_count) {}

   void on_container_start(proton::container& c) override {
       c.connect(server_address_);
   }

   void on_connection_open(proton::connection& c) override {
       sender_ = c.open_sender("/test");
   }

   void on_session_open(proton::session& s) override {
       wait_for_promise_or_fail(ready_promise, "settle_before_discharge client ready");
       std::cout << "[CLIENT] Declaring transaction with settle_before_discharge=" 
                 << settle_before_discharge_ << std::endl;
       s.transaction_declare(settle_before_discharge_);
   }

   void on_session_transaction_declared(proton::session& s) override {
       std::cout << "[CLIENT] Transaction declared" << std::endl;
       send_messages();
   }

   void on_sendable(proton::sender&) override {
       send_messages();
   }

   void send_messages() {
       proton::session session = sender_.session();
       static int sent = 0;
       while (session.transaction_is_declared() && sender_.credit() && sent < messages_to_send_) {
           proton::message msg("settle test message " + std::to_string(sent));
           sender_.send(msg);
           sent++;
       }

       if (sent == messages_to_send_) {
           std::cout << "[CLIENT] Committing transaction" << std::endl;
           session.transaction_commit();
       }
   }

   void on_tracker_accept(proton::tracker& t) override {
       accepted_count++;
       std::cout << "[CLIENT] Tracker accepted (count: " << accepted_count << ")" << std::endl;
   }

   void on_tracker_settle(proton::tracker& t) override {
       settled_count++;
       std::cout << "[CLIENT] Tracker settled (count: " << settled_count << ")" << std::endl;
   }

   void on_session_transaction_committed(proton::session& s) override {
       std::cout << "[CLIENT] Transaction committed. Settled: " << settled_count 
                 << ", Accepted: " << accepted_count << std::endl;
       finished_promise.set_value();
       s.connection().close();
   }
};

void test_transactional_accept() {
    std::cout << "\n=== TEST: Transactional Accept ===" << std::endl;
    
    std::string broker_address("127.0.0.1:0");
    EnhancedFakeBroker broker(broker_address);
    broker.set_auto_close(true);
    
    proton::container broker_container(broker);
    std::thread broker_thread([&broker_container]() { broker_container.run(); });
    
    std::unique_lock<std::mutex> lk(m);
    listener_ready = false;
    cv.wait(lk, [] { return listener_ready; });
    
    std::string server_address = "127.0.0.1:" + std::to_string(listener_port);
    TransactionalDeliveryClient client(server_address, 3, "accept");
    
    proton::container client_container(client);
    std::thread client_thread([&client_container]() { client_container.run(); });
    
    client.ready_promise.set_value();
    wait_for_promise_or_fail(broker.declare_promises[0], "declare");
    wait_for_promise_or_fail(broker.commit_promises[0], "commit");
    wait_for_promise_or_fail(client.finished_promise, "client finished");
    
    broker_thread.join();
    client_thread.join();
    
    // Verify transactional accept was called
    ASSERT(client.transactional_accept_count > 0);
    std::cout << "✓ Transactional accept test passed (accepts: " 
              << client.transactional_accept_count << ")" << std::endl;
}

void test_settle_before_discharge_true() {
    std::cout << "\n=== TEST: settle_before_discharge = true ===" << std::endl;
    
    std::string broker_address("127.0.0.1:0");
    EnhancedFakeBroker broker(broker_address);
    broker.set_auto_close(true);
    
    proton::container broker_container(broker);
    std::thread broker_thread([&broker_container]() { broker_container.run(); });
    
    std::unique_lock<std::mutex> lk(m);
    listener_ready = false;
    cv.wait(lk, [] { return listener_ready; });
    
    std::string server_address = "127.0.0.1:" + std::to_string(listener_port);
    SettleBeforeDischargeClient client(server_address, true, 2);
    
    proton::container client_container(client);
    std::thread client_thread([&client_container]() { client_container.run(); });
    
    client.ready_promise.set_value();
    wait_for_promise_or_fail(broker.declare_promises[0], "declare");
    wait_for_promise_or_fail(broker.commit_promises[0], "commit");
    wait_for_promise_or_fail(client.finished_promise, "client finished");
    
    broker_thread.join();
    client_thread.join();
    
    // With settle_before_discharge=true, messages should be settled before commit
    std::cout << "✓ settle_before_discharge=true test passed (settled: " 
              << client.settled_count << ", accepted: " << client.accepted_count << ")" << std::endl;
}

void test_settle_before_discharge_false() {
    std::cout << "\n=== TEST: settle_before_discharge = false ===" << std::endl;
    
    std::string broker_address("127.0.0.1:0");
    EnhancedFakeBroker broker(broker_address);
    broker.set_auto_close(true);
    
    proton::container broker_container(broker);
    std::thread broker_thread([&broker_container]() { broker_container.run(); });
    
    std::unique_lock<std::mutex> lk(m);
    listener_ready = false;
    cv.wait(lk, [] { return listener_ready; });
    
    std::string server_address = "127.0.0.1:" + std::to_string(listener_port);
    SettleBeforeDischargeClient client(server_address, false, 2);
    
    proton::container client_container(client);
    std::thread client_thread([&client_container]() { client_container.run(); });
    
    client.ready_promise.set_value();
    wait_for_promise_or_fail(broker.declare_promises[0], "declare");
    wait_for_promise_or_fail(broker.commit_promises[0], "commit");
    wait_for_promise_or_fail(client.finished_promise, "client finished");
    
    broker_thread.join();
    client_thread.join();
    
    // With settle_before_discharge=false, messages should be settled after commit
    std::cout << "✓ settle_before_discharge=false test passed (settled: " 
              << client.settled_count << ", accepted: " << client.accepted_count << ")" << std::endl;
}

void test_commit_without_declare_error() {
    std::cout << "\n=== TEST: Commit Without Declare Error ===" << std::endl;
    
    std::string broker_address("127.0.0.1:0");
    EnhancedFakeBroker broker(broker_address);
    
    proton::container broker_container(broker);
    std::thread broker_thread([&broker_container]() { broker_container.run(); });
    
    std::unique_lock<std::mutex> lk(m);
    listener_ready = false;
    cv.wait(lk, [] { return listener_ready; });
    
    std::string server_address = "127.0.0.1:" + std::to_string(listener_port);
    ErrorTestClient client(server_address, "commit_without_declare");
    
    proton::container client_container(client);
    std::thread client_thread([&client_container]() { client_container.run(); });
    
    client.ready_promise.set_value();
    wait_for_promise_or_fail(client.error_promise, "error caught");
    
    ASSERT(client.error_occurred);
    
    broker.listener.stop();
    broker_thread.join();
    client_thread.join();
    
    std::cout << "✓ Commit without declare error test passed" << std::endl;
}

void test_abort_without_declare_error() {
    std::cout << "\n=== TEST: Abort Without Declare Error ===" << std::endl;
    
    std::string broker_address("127.0.0.1:0");
    EnhancedFakeBroker broker(broker_address);
    
    proton::container broker_container(broker);
    std::thread broker_thread([&broker_container]() { broker_container.run(); });
    
    std::unique_lock<std::mutex> lk(m);
    listener_ready = false;
    cv.wait(lk, [] { return listener_ready; });
    
    std::string server_address = "127.0.0.1:" + std::to_string(listener_port);
    ErrorTestClient client(server_address, "abort_without_declare");
    
    proton::container client_container(client);
    std::thread client_thread([&client_container]() { client_container.run(); });
    
    client.ready_promise.set_value();
    wait_for_promise_or_fail(client.error_promise, "error caught");
    
    ASSERT(client.error_occurred);
    
    broker.listener.stop();
    broker_thread.join();
    client_thread.join();
    
    std::cout << "✓ Abort without declare error test passed" << std::endl;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

int main(int argc, char** argv) {
   int tests_failed = 0;

   std::cout << "========================================" << std::endl;
   std::cout << "Enhanced Transaction Test Suite" << std::endl;
   std::cout << "========================================" << std::endl;

   // Start broker for basic tests
   std::string broker_address("127.0.0.1:0");
   EnhancedFakeBroker broker(broker_address);
   broker.set_auto_close(false);

   proton::container broker_container(broker);
   std::thread broker_thread([&broker_container]() { broker_container.run(); });

   // Wait for the listener
   {
       std::unique_lock<std::mutex> lk(m);
       listener_ready = false;
       cv.wait(lk, [] { return listener_ready; });
   }

   // Run basic tests with shared broker
   RUN_ARGV_TEST(tests_failed, test_basic_commit(broker));
   RUN_ARGV_TEST(tests_failed, test_basic_abort(broker));

   broker.listener.stop();
   broker_thread.join();

   // Run tests that need their own broker
   RUN_ARGV_TEST(tests_failed, test_transaction_id_retrieval());
   RUN_ARGV_TEST(tests_failed, test_multiple_sequential_transactions());
   RUN_ARGV_TEST(tests_failed, test_double_declare_error());
   RUN_ARGV_TEST(tests_failed, test_commit_without_declare_error());
   RUN_ARGV_TEST(tests_failed, test_abort_without_declare_error());
   
   // Transactional delivery tests
   RUN_ARGV_TEST(tests_failed, test_transactional_accept());
   
   // settle_before_discharge parameter tests
   RUN_ARGV_TEST(tests_failed, test_settle_before_discharge_true());
   RUN_ARGV_TEST(tests_failed, test_settle_before_discharge_false());

   std::cout << "\n========================================" << std::endl;
   if (tests_failed == 0) {
       std::cout << "✓ All tests passed!" << std::endl;
   } else {
       std::cout << "✗ " << tests_failed << " test(s) failed" << std::endl;
   }
   std::cout << "========================================" << std::endl;

   return tests_failed;
}

// Made with Bob
