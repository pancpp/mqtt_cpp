// Copyright Takatoshi Kondo 2020
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// no_tls client and server

#include <iostream>
#include <iomanip>
#include <map>

#include <mqtt_client_cpp.hpp>

#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>
#include <boost/date_time/posix_time/posix_time_io.hpp>

#include "locked_cout.hpp"

template <typename Client, typename Disconnect>
void client_proc(
    Client& c,
    std::uint16_t& pid_sub1,
    std::uint16_t& pid_sub2,
    Disconnect const& disconnect) {

    using packet_id_t = typename std::remove_reference_t<decltype(*c)>::packet_id_t;
    // Setup client
    c->set_client_id("cid1");
    c->set_clean_session(true);

    // Setup handlers
    c->set_connack_handler(
        [&c, &pid_sub1, &pid_sub2]
        (bool sp, MQTT_NS::connect_return_code connack_return_code){
            locked_cout() << "[client] Connack handler called" << std::endl;
            locked_cout() << "[client] Session Present: " << std::boolalpha << sp << std::endl;
            locked_cout() << "[client] Connack Return Code: "
                      << MQTT_NS::connect_return_code_to_str(connack_return_code) << std::endl;
            if (connack_return_code == MQTT_NS::connect_return_code::accepted) {
                pid_sub1 = c->subscribe("mqtt_client_cpp/topic1", MQTT_NS::qos::at_most_once);
                pid_sub2 = c->subscribe(
                    std::vector<std::tuple<MQTT_NS::string_view, MQTT_NS::subscribe_options>>
                    {
                        { "mqtt_client_cpp/topic2_1", MQTT_NS::qos::at_least_once },
                        { "mqtt_client_cpp/topic2_2", MQTT_NS::qos::exactly_once }
                    }
                );
            }
            return true;
        });
    c->set_close_handler(
        []
        (){
            locked_cout() << "[client] closed." << std::endl;
        });
    c->set_error_handler(
        []
        (MQTT_NS::error_code ec){
            locked_cout() << "[client] error: " << ec.message() << std::endl;
        });
    c->set_puback_handler(
        [&]
        (packet_id_t packet_id){
            locked_cout() << "[client] puback received. packet_id: " << packet_id << std::endl;
            disconnect();
            return true;
        });
    c->set_pubrec_handler(
        [&]
        (packet_id_t packet_id){
            locked_cout() << "[client] pubrec received. packet_id: " << packet_id << std::endl;
            return true;
        });
    c->set_pubcomp_handler(
        [&]
        (packet_id_t packet_id){
            locked_cout() << "[client] pubcomp received. packet_id: " << packet_id << std::endl;
            disconnect();
            return true;
        });
    c->set_suback_handler(
        [&]
        (packet_id_t packet_id, std::vector<MQTT_NS::suback_return_code> results){
            locked_cout() << "[client] suback received. packet_id: " << packet_id << std::endl;
            for (auto const& e : results) {
                locked_cout() << "[client] subscribe result: " << e << std::endl;
            }
            if (packet_id == pid_sub1) {
                c->publish("mqtt_client_cpp/topic1", "test1", MQTT_NS::qos::at_most_once);
            }
            else if (packet_id == pid_sub2) {
                c->publish("mqtt_client_cpp/topic2_1", "test2_1", MQTT_NS::qos::at_least_once);
                c->publish("mqtt_client_cpp/topic2_2", "test2_2", MQTT_NS::qos::exactly_once);
            }
            return true;
        });
    c->set_publish_handler(
        [&]
        (MQTT_NS::optional<packet_id_t> packet_id,
         MQTT_NS::publish_options pubopts,
         MQTT_NS::buffer topic_name,
         MQTT_NS::buffer contents){
            locked_cout() << "[client] publish received. "
                          << " dup: "    << pubopts.get_dup()
                          << " qos: "    << pubopts.get_qos()
                          << " retain: " << pubopts.get_retain() << std::endl;
            if (packet_id)
                locked_cout() << "[client] packet_id: " << *packet_id << std::endl;
            locked_cout() << "[client] topic_name: " << topic_name << std::endl;
            locked_cout() << "[client] contents: " << contents << std::endl;
            disconnect();
            return true;
        });

    // Connect
    c->connect();
}

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>
#include <mqtt_server_cpp.hpp>

namespace mi = boost::multi_index;

using con_t = MQTT_NS::server<>::endpoint_t;
using con_sp_t = std::shared_ptr<con_t>;

struct sub_con {
    sub_con(MQTT_NS::buffer topic, con_sp_t con, MQTT_NS::qos qos_value)
        :topic(std::move(topic)), con(std::move(con)), qos_value(qos_value) {}
    MQTT_NS::buffer topic;
    con_sp_t con;
    MQTT_NS::qos qos_value;
};

struct tag_topic {};
struct tag_con {};

using mi_sub_con = mi::multi_index_container<
    sub_con,
    mi::indexed_by<
        mi::ordered_non_unique<
            mi::tag<tag_topic>,
            BOOST_MULTI_INDEX_MEMBER(sub_con, MQTT_NS::buffer, topic)
        >,
        mi::ordered_non_unique<
            mi::tag<tag_con>,
            BOOST_MULTI_INDEX_MEMBER(sub_con, con_sp_t, con)
        >
    >
>;


inline void close_proc(std::set<con_sp_t>& cons, mi_sub_con& subs, con_sp_t const& con) {
    cons.erase(con);

    auto& idx = subs.get<tag_con>();
    auto r = idx.equal_range(con);
    idx.erase(r.first, r.second);
}

template <typename Server>
void server_proc(Server& s, std::set<con_sp_t>& connections, mi_sub_con& subs) {
    s.set_error_handler(
        [](MQTT_NS::error_code ec) {
            locked_cout() << "[server] error: " << ec.message() << std::endl;
        }
    );
    s.set_accept_handler(
        [&s, &connections, &subs](con_sp_t spep) {
            auto& ep = *spep;
            std::weak_ptr<con_t> wp(spep);

            using packet_id_t = typename std::remove_reference_t<decltype(ep)>::packet_id_t;
            locked_cout() << "[server] accept" << std::endl;
            // For server close if ep is closed.
            auto g = MQTT_NS::shared_scope_guard(
                [&s] {
                    locked_cout() << "[server] session end" << std::endl;
                    s.close();
                }
            );

            // Pass spep to keep lifetime.
            // It makes sure wp.lock() never return nullptr in the handlers below
            // including close_handler and error_handler.
            ep.start_session(std::make_tuple(std::move(spep), std::move(g)));

            // set connection (lower than MQTT) level handlers
            ep.set_close_handler(
                [&connections, &subs, wp]
                (){
                    locked_cout() << "[server] closed." << std::endl;
                    auto sp = wp.lock();
                    BOOST_ASSERT(sp);
                    close_proc(connections, subs, sp);
                });
            ep.set_error_handler(
                [&connections, &subs, wp]
                (MQTT_NS::error_code ec){
                    locked_cout() << "[server] error: " << ec.message() << std::endl;
                    auto sp = wp.lock();
                    BOOST_ASSERT(sp);
                    close_proc(connections, subs, sp);
                });

            // set MQTT level handlers
            ep.set_connect_handler(
                [&connections, wp]
                (MQTT_NS::buffer client_id,
                 MQTT_NS::optional<MQTT_NS::buffer> username,
                 MQTT_NS::optional<MQTT_NS::buffer> password,
                 MQTT_NS::optional<MQTT_NS::will>,
                 bool clean_session,
                 std::uint16_t keep_alive) {
                    using namespace MQTT_NS::literals;
                    locked_cout() << "[server] client_id    : " << client_id << std::endl;
                    locked_cout() << "[server] username     : " << (username ? username.value() : "none"_mb) << std::endl;
                    locked_cout() << "[server] password     : " << (password ? password.value() : "none"_mb) << std::endl;
                    locked_cout() << "[server] clean_session: " << std::boolalpha << clean_session << std::endl;
                    locked_cout() << "[server] keep_alive   : " << keep_alive << std::endl;
                    auto sp = wp.lock();
                    BOOST_ASSERT(sp);
                    connections.insert(sp);
                    sp->connack(false, MQTT_NS::connect_return_code::accepted);
                    return true;
                }
            );
            ep.set_disconnect_handler(
                [&connections, &subs, wp]
                (){
                    locked_cout() << "[server] disconnect received." << std::endl;
                    auto sp = wp.lock();
                    BOOST_ASSERT(sp);
                    close_proc(connections, subs, sp);
                });
            ep.set_puback_handler(
                []
                (packet_id_t packet_id){
                    locked_cout() << "[server] puback received. packet_id: " << packet_id << std::endl;
                    return true;
                });
            ep.set_pubrec_handler(
                []
                (packet_id_t packet_id){
                    locked_cout() << "[server] pubrec received. packet_id: " << packet_id << std::endl;
                    return true;
                });
            ep.set_pubrel_handler(
                []
                (packet_id_t packet_id){
                    locked_cout() << "[server] pubrel received. packet_id: " << packet_id << std::endl;
                    return true;
                });
            ep.set_pubcomp_handler(
                []
                (packet_id_t packet_id){
                    locked_cout() << "[server] pubcomp received. packet_id: " << packet_id << std::endl;
                    return true;
                });
            ep.set_publish_handler(
                [&subs]
                (MQTT_NS::optional<packet_id_t> packet_id,
                 MQTT_NS::publish_options pubopts,
                 MQTT_NS::buffer topic_name,
                 MQTT_NS::buffer contents){
                    locked_cout() << "[server] publish received."
                                  << " dup: "    << pubopts.get_dup()
                                  << " qos: "    << pubopts.get_qos()
                                  << " retain: " << pubopts.get_retain() << std::endl;
                    if (packet_id)
                        locked_cout() << "[server] packet_id: " << *packet_id << std::endl;
                    locked_cout() << "[server] topic_name: " << topic_name << std::endl;
                    locked_cout() << "[server] contents: " << contents << std::endl;
                    auto const& idx = subs.get<tag_topic>();
                    auto r = idx.equal_range(topic_name);
                    for (; r.first != r.second; ++r.first) {
                        r.first->con->publish(
                            topic_name,
                            contents,
                            std::min(r.first->qos_value, pubopts.get_qos())
                        );
                    }
                    return true;
                });
            ep.set_subscribe_handler(
                [&subs, wp]
                (packet_id_t packet_id,
                 std::vector<std::tuple<MQTT_NS::buffer, MQTT_NS::subscribe_options>> entries) {
                    locked_cout() << "[server]subscribe received. packet_id: " << packet_id << std::endl;
                    std::vector<MQTT_NS::suback_return_code> res;
                    res.reserve(entries.size());
                    auto sp = wp.lock();
                    BOOST_ASSERT(sp);
                    for (auto const& e : entries) {
                        MQTT_NS::buffer topic = std::get<0>(e);
                        MQTT_NS::qos qos_value = std::get<1>(e).get_qos();
                        locked_cout() << "[server] topic: " << topic  << " qos: " << qos_value << std::endl;
                        res.emplace_back(MQTT_NS::qos_to_suback_return_code(qos_value));
                        subs.emplace(std::move(topic), sp, qos_value);
                    }
                    sp->suback(packet_id, res);
                    return true;
                }
            );
            ep.set_unsubscribe_handler(
                [&subs, wp]
                (packet_id_t packet_id,
                 std::vector<MQTT_NS::buffer> topics) {
                    locked_cout() << "[server]unsubscribe received. packet_id: " << packet_id << std::endl;
                    for (auto const& topic : topics) {
                        subs.erase(topic);
                    }
                    auto sp = wp.lock();
                    BOOST_ASSERT(sp);
                    sp->unsuback(packet_id);
                    return true;
                }
            );
        }
    );

    s.listen();
}

inline
void setup_log() {
    // https://www.boost.org/doc/libs/1_73_0/libs/log/doc/html/log/tutorial/advanced_filtering.html

    auto fmt =
        [](boost::log::record_view const& rec, boost::log::formatting_ostream& strm) {
            // Timestamp custom formatting example
            if (auto v = boost::log::extract<boost::posix_time::ptime>("TimeStamp", rec)) {
                strm.imbue(
                    std::locale(
                        strm.getloc(),
                        // https://www.boost.org/doc/html/date_time/date_time_io.html#date_time.format_flags
                        new boost::posix_time::time_facet("%H:%M:%s") // ownership is moved here
                    )
                );
                strm << v.get() << " ";
            }
            // ThreadID indexed example
            if (auto v = boost::log::extract<boost::log::thread_id>("ThreadID", rec)) {
                static std::map<boost::log::thread_id, std::size_t> m;
                auto it = m.find(v.get());
                if (it == m.end()) {
                    auto r = m.emplace(v.get(), m.size());
                    BOOST_ASSERT(r.second);
                    it = r.first;
                }
                strm << "TID[" << it->second << "] ";
            }
            // Adjust severity length example
            if (auto v = boost::log::extract<MQTT_NS::severity_level>("MqttSeverity", rec)) {
                strm << "SEV[" << std::setw(7) << std::left << v.get() << "] ";
            }
            if (auto v = boost::log::extract<boost::log::trivial::severity_level>("Severity", rec)) {
                strm << "SEV[" << v.get() << "] ";
            }
            if (auto v = boost::log::extract<std::string>("MqttChannel", rec)) {
                strm << "CHANNEL[" << std::setw(5) << std::left << v.get() << "] ";
            }
            // Shorten file path example
            if (auto v = boost::log::extract<std::string>("MqttFile", rec)) {
                strm << boost::filesystem::path(v.get()).filename().string() << ":";
            }
            if (auto v = boost::log::extract<unsigned int>("MqttLine", rec)) {
                strm << v.get() << " ";
            }
            if (auto v = boost::log::extract<void const*>("MqttAddress", rec)) {
                strm << "ADDR[" << v.get() << "] ";
            }
#if 0
            if (auto v = boost::log::extract<std::string>("MqttFunction", rec)) {
                strm << v << ":";
            }
#endif
            strm << rec[boost::log::expressions::smessage];
        };

    // https://www.boost.org/doc/libs/1_73_0/libs/log/doc/html/log/tutorial/sinks.html
    boost::shared_ptr<std::ostream> stream(&std::clog, boost::null_deleter());

    using text_sink = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;
    auto sink = boost::make_shared<text_sink>();
    sink->locked_backend()->add_stream(stream);
    sink->set_formatter(fmt);

    auto fil =
        [](boost::log::attribute_value_set const& avs) {
            {
                // For mqtt
                std::map<std::string, MQTT_NS::severity_level> threshold {
                    {
                        { "api", MQTT_NS::severity_level::trace },
                        { "cb", MQTT_NS::severity_level::debug },
                    }
                };
                auto channel = boost::log::extract<std::string>("MqttChannel", avs);
                auto severity = boost::log::extract<MQTT_NS::severity_level>("MqttSeverity", avs);
                if (channel && severity) {
                    return severity.get() >= threshold[channel.get()];
                }
            }
            {
                // For default
                auto severity = boost::log::extract<boost::log::trivial::severity_level>("Severity", avs);
                return severity.get() >= boost::log::trivial::warning;
            }
            return true;
        };
    sink->set_filter(fil);

    boost::log::core::get()->add_sink(sink);

    boost::log::add_common_attributes();
}


int main(int argc, char** argv) {
    if (argc != 2) {
        locked_cout() << argv[0] << " port" << std::endl;
        return -1;
    }

    setup_log();

    boost::asio::io_context ioc;
    std::uint16_t port = boost::lexical_cast<std::uint16_t>(argv[1]);

    // server
    boost::asio::io_context iocs;
    auto s = MQTT_NS::server<>(
        boost::asio::ip::tcp::endpoint(
            boost::asio::ip::tcp::v4(),
            boost::lexical_cast<std::uint16_t>(argv[1])
        ),
        iocs
    );
    std::set<con_sp_t> connections;
    mi_sub_con subs;
    std::thread th(
        [&] {
            server_proc(s, connections, subs);
            iocs.run();
        }
    );


    // client
    std::uint16_t pid_sub1;
    std::uint16_t pid_sub2;

    auto c = MQTT_NS::make_sync_client(ioc, "localhost", port);

    int count = 0;
    auto disconnect = [&] {
        if (++count == 5) c->disconnect();
    };
    client_proc(c, pid_sub1, pid_sub2, disconnect);


    ioc.run();
    th.join();
}