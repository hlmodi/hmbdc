#include "hmbdc/tips/Tips.hpp"
#include "hmbdc/time/Timers.hpp"
#include "hmbdc/tips/tcpcast/Protocol.hpp"
#include "hmbdc/tips/rmcast/Protocol.hpp"
#include "hmbdc/tips/rnetmap/Protocol.hpp"

#include <boost/format.hpp>
#include <boost/program_options.hpp>

#include <unordered_set>
#include <iostream>
#include <iomanip>
#include <memory>
#include <unistd.h>
#include <atomic>


using namespace std;
using namespace boost;
using namespace hmbdc;
using namespace hmbdc::tips;
using namespace hmbdc::app;

namespace hmbdc { namespace tips {
struct BagHead {
    time::SysTime createdTs;
    size_t cfgLen;

    friend
    std::ostream& operator << (std::ostream& os, BagHead const& h) {
        return os.write((char const*)&h, sizeof(h));
    }
    friend
    std::istream& operator >> (std::istream& is, BagHead& h) {
        return is.read((char*)&h, sizeof(h));
    }
};

struct BagMessageHead {
    time::SysTime createdTs;
    size_t attLen;
    uint16_t tag;
    uint16_t reserved;
    uint32_t reserved2;

    friend
    std::ostream& operator << (std::ostream& os, BagMessageHead const& h) {
        return os.write((char const*)&h, sizeof(h));
    }
    friend
    std::istream& operator >> (std::istream& is, BagMessageHead& h) {
        return is.read((char*)&h, sizeof(h));
    }
};

/**
 * @class ConsoleNode<>
 * @brief a Node that work as a console to send and receive messages
 * @details a Node that receives all messages delivered to the process
 *  it uses JustBytes to indicate it wants all the messages bytes
 * regardless of their types
 * 
 * @tparam Domain some NetProp type for a specific transport mechanism, 
 * for example udpcast::NetProp, tcpcast::NetProp
 */
template <typename Domain>
struct ConsoleNode 
: Node<ConsoleNode<Domain>, std::tuple<JustBytes>>
, time::TimerManager {
    using SendMessageTuple = std::tuple<JustBytes>;
    /**
     * @brief constructor
     * @details just construct the Node - need to be run in the Context later
     * 
     * @param domain a runtime sized context that the Node will run in
     * @param myCin for incoming commands 
     * @param myCout for output
     * @param myCerr for error
     */
    ConsoleNode(Domain& domain
        , Config const& cfg
        , istream& myCin = cin
        , ostream& myCout = cout
        , ostream& myCerr = cerr) 
    : domain_(domain)
    , config_(cfg)
    , hex_(true)
    , myCin_(myCin)
    , myCout_(myCout)
    , myCerr_(myCerr)
    , bufWidth_(cfg.getExt<size_t>("bufWidth"))
    , toSend_(new uint8_t[bufWidth_]) {
        thread stdinThread([this]() {
            stdinThreadEntrance();
        });
        stdinThread_ = std::move(stdinThread);
    }

    char const* hmbdcName() const { 
        return "console"; 
    }

    size_t maxMessageSize() const {
        return bufWidth_;
    }

    /**
     * @brief documentation for all commands the console interprets
     * @details commands are seperated by \n, when read eof, terminates console
     * @return a documentation string 
     */
    static char const* help() {
        return 
R"|(CONSOLE LANGUAGE:
This console accept control cmds and messages from stdin and send output to stdout. stderr has errors.
Pub/sub cmds and example:
pubtags <space-seperated-tags>
    example: pubtags 1001 1004 1002
subtags <space-seperated-tags>
    example: subtags 2001 1004 11002
pubstr <tag> <string>
    example: pubstr 1099 some string in a line
pub <tag> <msg-len> <space-seprated-hex-for-msg>
    example: pub 1251 4 a1 a2 a3 a4
pubatt <tag> <msg-len> <attachment-len> <space-seprated-hex-for-msg-followed-by-attachment>
    example: pubatt 1201 4 10 01 02 03 04 01 02 03 04 05 06 07 08 09 0a
play <bag-file-name>
    play a recorded data bag (in the same relative timing as recorded)

Output control cmds:
ohex
    output bytes in hex - default. There is always a '\n' between the message bytes and attachment bytes.
    ohex output example:
    1201 msg= 02 03 04 ff ff ff ff ff ff ff ff ff ff ff
    att= 01 02 03 04 05 06 07 08 09 0a

ostr
    output in string - unless you are sure all the incoming messages are strings without attachment, do not use this.
    ostr output example:
    1099 some string in a line

record <bag-file-name> <duration>
    record the output within the next duration seconds with time info in binary bag format into a file (bag) and exit
    example:
    record /tmp/a.bag 99.5

Other cmds:
exit
    exit

)|"
;
    }

    void stoppedCb(std::exception const& e) override {
        myCerr_ << "[status] " << (e.what()) << endl;
    };

    void addJustBytesPubsFor(std::function<void(uint16_t)> addTag) const {
        for (auto tag : pubTags_) {
            addTag(tag);
        }
    }

    void addJustBytesSubsFor(std::function<void(uint16_t)> addTag) const {
        for (auto tag : subTags_) {
            addTag(tag);
        }
    }

    void invokedCb(size_t) override {
        if (recordBagOpen_ && !recordBagCloseTimer_.scheduled()) {
            this->schedule(time::SysTime::now() + recordDuration_, recordBagCloseTimer_);
        }

        if (toPlayTag_ == 0 && playBagOpen_ && !playBagFireTimer_.scheduled()) {
            /// schedule for the first time
            schedule(time::SysTime::now(), playBagFireTimer_);
        }
    }

    void handleJustBytesCb(uint16_t tag, uint8_t* bytes, hasMemoryAttachment* att) {
        //only handle user msgs
        if (tag <= LastSystemMessage::typeTag) return;

        if (recordBagOpen_) {
            BagMessageHead bh{time::SysTime::now(), att?att->len:0xfffffffffffffffful, tag};
            recordBag_ << bh;
            recordBag_.write((char const*)bytes, bufWidth_);
            if (att) {
                recordBag_.write((char*)att->attachment, att->len);
            }
        } else {
            myCout_ << dec << tag << " msg=";
            if (att) {
                bytes += sizeof(app::hasMemoryAttachment);
            }
            if (hex_) {
                for (auto p = bytes; p != bytes + bufWidth_ - sizeof(app::hasMemoryAttachment); ++p) {
                    myCout_ << ' ' << boost::format("%02x") % (uint16_t)*p;
                }
                if (att) {
                    myCout_ << "\natt=";
                    for (auto p = (uint8_t*)att->attachment; p != (uint8_t*)att->attachment + att->len; ++p) {
                        myCout_ << ' ' << boost::format("%02x") % (uint16_t)*p;
                    }
                    att->release(); /// important for JustBytes
                }
            } else {
                myCout_ << ' ' << string((char*)bytes, strnlen((char*)bytes, bufWidth_));
                if (att) {
                    myCerr_ << "[status] ignored att for tag=" << tag << endl;
                }
            }
            myCout_ << endl;
        }
    }

    /**
     * @brief wait until the console closes
     */
    void waitUntilFinish() {
        stdinThread_.join();
    }

 private:
    void stdinThreadEntrance() {
        myCerr_ << "[status] Session started" << std::endl;
        processCmd(myCin_);
        myCerr_ << "[status] Session stopped" << std::endl;
    }

    void processCmd(istream& is) {
        string line; 
        while(getline(is, line)) {
            if (!line.size()) continue;
            istringstream iss{line};
            string op;
            iss >> op;
            if (op == "pubtags") {
                for (auto it = std::istream_iterator<uint16_t>(iss)
                    ; it != std::istream_iterator<uint16_t>()
                    ; ++it) {
                    pubTags_.insert(*it);
                };
                domain_.addPubSubFor(*this);
            } else if (op == "subtags") {
                for (auto it = std::istream_iterator<uint16_t>(iss)
                    ; it != std::istream_iterator<uint16_t>()
                    ; ++it) {
                    subTags_.insert(*it);
                    this->subscription.set(*it);
                };
                domain_.addPubSubFor(*this);
            } else if (op == "pubstr") {
                uint16_t tag;
                iss >> tag;
                string msg;
                getline(iss, msg); /// msg has a leading space
                auto len = msg.size();
                if (iss && len >= 0 && pubTags_.find(tag) != pubTags_.end()) {
                    domain_.publishJustBytes(tag, msg.c_str() + 1, (size_t)len, nullptr);
                } else {
                    myCerr_ << "[status] pubstr syntax error or unknown tag" << endl;
                }
            } else if (op == "pub") {
                uint16_t tag;
                iss >> tag;
                size_t msgLen;
                iss >> msgLen;
                iss >> hex;
                if (msgLen > bufWidth_) {
                    myCerr_ << "[status] msgLen too big" << endl;
                    continue;
                }
                auto msgBytes = &toSend_[0];
                for (auto i = 0u; iss && i < msgLen; ++i) {
                    uint16_t byte;
                    iss >> byte;
                    msgBytes[i] = (uint8_t)byte;
                };
                if (iss && pubTags_.find(tag) != pubTags_.end()) {
                    domain_.publishJustBytes(tag, toSend_.get(), msgLen, nullptr);
                } else {
                    myCerr_ << "[status] syntax error or unknown tag" << endl;
                }
            } else if (op == "pubatt") {
                uint16_t tag;
                iss >> tag;
                size_t msgLen, attLen;
                iss >> msgLen >> attLen;
                iss >> hex;
                auto att = new (toSend_.get()) app::hasMemoryAttachment;
                if (msgLen + sizeof(*att) > bufWidth_) {
                    myCerr_ << "[status] msgLen too big" << endl;
                    continue;
                }
                auto msgBytes = &toSend_[sizeof(*att)];
                for (auto i = 0u; iss && i < msgLen; ++i) {
                    uint16_t byte;
                    iss >> byte;
                    msgBytes[i] = (uint8_t)byte;
                };
                if (iss) {
                    att->len = attLen;
                    att->attachment = malloc(sizeof(size_t) + att->len);
                    auto& refCount = *(size_t*)att->attachment;
                    /// based on what we know set the release called times
                    /// to be 2 or 1
                    refCount = this->subscription.check(tag) ? 2 : 1;
                    att->clientData[0] = (uint64_t)&refCount;
                    att->attachment = (char*)att->attachment + sizeof(size_t);
                    att->afterConsumedCleanupFunc = [](app::hasMemoryAttachment* att) {
                        auto& refCount = *((size_t*)att->clientData[0]);
                        if (0 == __atomic_sub_fetch(&refCount, 1, __ATOMIC_RELAXED)) {
                            auto toFree = (char*)att->attachment;
                            toFree -= sizeof(size_t);
                            ::free(toFree);
                        }
                    };

                    for (auto i = 0u; iss && i < attLen; ++i) {
                        uint16_t byte;
                        iss >> byte;
                        ((uint8_t*)att->attachment)[i] = (uint8_t)byte;
                    };
                }
                if (iss && pubTags_.find(tag) != pubTags_.end()) {
                    domain_.publishJustBytes(tag, toSend_.get(), msgLen + sizeof(*att), att);
                } else {
                    myCerr_ << "[status] syntax error or unknown tag" << endl;
                    auto toFree = (char*)att->attachment;
                    toFree -= sizeof(size_t);
                    ::free(toFree);
                    // att->release();
                }
            } else if (op == "record") {
                std::string bagName;
                iss >> bagName >> recordDuration_;
                if (!iss) {
                    myCerr_ << "[status] syntax error " << endl;
                } else if (recordBagOpen_) {
                    myCerr_ << "[status] error, already recording! " << endl;
                } else {
                    recordBag_.open(bagName, std::ofstream::out | std::ios::binary);
                    std::ostringstream oss; 
                    boost::property_tree::write_json(oss, config_);
                    auto cfgText = oss.str();
                    size_t l = cfgText.size();
                    BagHead bh{time::SysTime::now(), l};
                    recordBag_ << bh;
                    recordBag_.write(cfgText.c_str(), cfgText.size());
                    if (!recordBag_) {
                        myCerr_ << "[status] error openning bag! " << endl;
                    } else {
                        recordBagOpen_ = true;
                    }
                }
            } else if (op == "play") {
                std::string bagName;
                iss >> bagName;
                if (!iss) {
                    myCerr_ << "[status] syntax error " << endl;
                } else if (playBagOpen_) {
                    myCerr_ << "[status] error, already playing! " << endl;
                } else {
                    playBag_.open(bagName, std::ofstream::in | std::ios::binary); 
                    BagHead bh; 
                    playBag_ >> bh; 
                    playBagPreviousTs_ = bh.createdTs;
                    std::ostringstream oss;
                    char conf[bh.cfgLen + 1];
                    playBag_.read(conf, bh.cfgLen);
                    conf[bh.cfgLen] = 0;
                    auto config = app::Config(conf);
                    toPlayLen_ = config.getExt<size_t>("bufWidth");
                    if (toPlayLen_ > bufWidth_) {
                        myCerr_ << "[status] this console's bufWidth < " << toPlayLen_ 
                            << " cannot play this bag " << endl;
                        playBag_.close();
                        continue;
                    }
                    toPlay_.reset(new uint8_t[toPlayLen_]);
                    playBagOpen_ = true;
                }
            } else if (op == "ohex") {
                hex_ = true;
            } else if (op == "ostr") {
                hex_ = false;
            } else if (op == "help") {
                myCout_ << help() << std::endl;
            } else if (op == "exit") {
                break;
            } else {
                myCerr_ << "[status] unknown command " << op << endl;
            }
        }
    }

    Domain& domain_;
    Config config_;
    atomic<bool> hex_;
    thread stdinThread_;
    istream& myCin_;
    ostream& myCout_;
    ostream& myCerr_;
    ofstream recordBag_;
    time::Duration recordDuration_;
    atomic<bool> recordBagOpen_ = false;
    time::OneTimeTimer recordBagCloseTimer_{[this](auto&& tm, auto&& now){
        recordBag_.close();
        recordBagOpen_ = false;
        myCerr_ << "[status] record bag ready! hit Enter to exit" << std::endl;
        domain_.stop();
        myCin_.setstate(std::ios::eofbit);
        throw 0;
    }};

    ifstream playBag_;
    time::SysTime playBagPreviousTs_;
    atomic<bool> playBagOpen_ = false;
    time::ReoccuringTimer playBagFireTimer_{time::Duration::seconds(0)
        , [this](auto&& tm, auto&& firedAt) {
            if (toPlayTag_) {
                domain_.publishJustBytes(
                    toPlayTag_, toPlay_.get(), toPlayLen_, toPlayAtt_);
            }
            BagMessageHead bmh;
            playBag_ >> bmh;
            if (playBag_) {
                toPlayTag_ = bmh.tag;
                playBagFireTimer_.resetInterval(bmh.createdTs - playBagPreviousTs_);
                playBagPreviousTs_ = bmh.createdTs;
                playBag_.read((char*)toPlay_.get(), toPlayLen_);
                if (bmh.attLen != 0xfffffffffffffffful) {
                    toPlayAtt_ = new (toPlay_.get()) app::hasMemoryAttachment;
                    toPlayAtt_->len = bmh.attLen;
                    toPlayAtt_->attachment = malloc(sizeof(size_t) + toPlayAtt_->len);
                    auto& refCount = *(size_t*)toPlayAtt_->attachment;
                    /// based on what we know set the release called times
                    /// to be 2 or 1
                    refCount = this->subscription.check(bmh.tag) ? 2 : 1;
                    toPlayAtt_->clientData[0] = (uint64_t)&refCount;
                    toPlayAtt_->attachment = (char*)toPlayAtt_->attachment + sizeof(size_t);
                    toPlayAtt_->afterConsumedCleanupFunc = [](app::hasMemoryAttachment* att) {
                        auto& refCount = *((size_t*)att->clientData[0]);
                        if (0 == __atomic_sub_fetch(&refCount, 1, __ATOMIC_RELAXED)) {
                            auto toFree = (char*)att->attachment;
                            toFree -= sizeof(size_t);
                            ::free(toFree);
                        }
                    };

                    if (!playBag_.read((char*)toPlayAtt_->attachment, toPlayAtt_->len)) {
                        myCerr_ << "[status] IO error when reading bag message" << std::endl;
                        toPlayAtt_->release();
                        playBag_.close();
                        playBagOpen_ = false;
                    }
                } else {
                    toPlayAtt_ = nullptr;
                }
            } else {
                if (playBag_.eof()) {
                    myCerr_ << "[status] bag play done " << std::endl;
                } else {
                    myCerr_ << "[status] IO error when reading bag head " << std::endl;
                }
                playBag_.close();
                playBagOpen_ = false;
                tm.cancel(playBagFireTimer_);
                toPlayTag_ = 0;
            }
        }
    };

    std::unordered_set<uint16_t> subTags_;
    std::unordered_set<uint16_t> pubTags_;
    size_t bufWidth_;
    std::unique_ptr<uint8_t[]> toSend_;

    uint16_t toPlayTag_ = 0;
    size_t toPlayLen_ = 0;
    std::unique_ptr<uint8_t[]> toPlay_;
    app::hasMemoryAttachment* toPlayAtt_ = nullptr;
};

}}

template <typename NetProt, uint32_t IpcCapacity>
struct Runner {
    int help(std::string const& netProt, uint32_t ipcCapacity) {
        if (netProt != NetProt::name() || ipcCapacity != IpcCapacity) {
            return 0;
        }
        cout << NetProt::name() << " configurations:\n";
        cout << NetProt::dftConfig() << "\n";
        cout << "IPC configurations:\n";
        cout << tips::DefaultUserConfig << "\n";
        return 0;
    }

    int operator()(std::string const& netProt, uint32_t ipcCapacity
        , Config const& config) {
        if (netProt != NetProt::name() || ipcCapacity != IpcCapacity) {
            return 0;
        }
        auto bufDepth = config.template getExt<size_t>("bufDepth");
        
        pattern::SingletonGuardian<NetProt> ncGuard;
        using ConsoleDomain 
            = Domain<std::tuple<JustBytes>, ipc_property<IpcCapacity, 0>, net_property<NetProt, 0>>;
        using Console = ConsoleNode<ConsoleDomain>;
        ConsoleDomain domain{config};
        Console console{domain, config};
        domain.start(console
            , bufDepth
            , time::Duration::seconds(1)
        );
        console.waitUntilFinish();
        domain.stop();
        domain.join();
        return 0;
    }
};

int main(int argc, char** argv) {
    pattern::SingletonGuardian<SyncLogger> logGuard(std::cerr);

    namespace po = boost::program_options;
    po::options_description desc("Allowed cmd options");
    auto helpStr = 
    R"|(
This program provides a console for a TIPS domain.
)|";
    desc.add_options()
    ("help,h", helpStr)
    ;

    std::string cfgString =
R"|(
{
    "ifaceAddr"     : "127.0.0.1",      "__ifaceAddr"   : "(non-rnetmap) ip address for the NIC interface for IO, 0.0.0.0/0 pointing to the first intereface that is not a loopback",
    "netmapPort"    : "UNSPECIFIED",    "__netmapPort"  : "(rnemtap) the netmap device (i.e. netmap:eth0-2, netmap:ens4) for sending/receiving, no default value. When multiple tx rings exists for a device (like 10G NIC), the sender side must be specific on which tx ring to use",
    "ipcCapacity"   : 64,               "__ipcCapacity" : "See capacity in ipc_property in Domain.hpp: 4 8 ... 256  - this needs to match ipc_property's IpcCapacity value of the monitored system",
    "netProt"       : "tcpcast",        "__netProt"     : "tcpcast udpcast rmcast netmap rnetmap",
    "bufDepth"      : 1024,             "__bufDepth"    : "console incoming buffer size",
    "bufWidth"      : 1000,             "__bufWidth"    : "max message that the console handles (excluding attachment) - this needs to match ipcMaxMessageSizeRuntime value of the monitored system",
    "logLevel"      : 3,                "__logLevel"    : "only log Warning and above by default"
}
)|";

    Config config(cfgString.c_str());
    auto params = config.content();
    for (auto it = params.begin(); it != params.end();) {
    	string& name = it->first;
    	string& val = it->second;
    	it++;
    	string& comment = it->second;
    	it++;
    	desc.add_options()
    		(name.c_str(), po::value<string>(&val)->default_value(val), comment.c_str());
    }

    po::positional_options_description p;
    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv).
        options(desc).positional(p).run(), vm);
    po::notify(vm);

#define RUNNER_LIST(x) Runner<x::Protocol, 4>, Runner<x::Protocol, 8>, Runner<x::Protocol, 16>, Runner<x::Protocol, 64>, Runner<x::Protocol, 128>, Runner<x::Protocol, 256>
    using Runners = std::tuple<
          RUNNER_LIST(tcpcast)
        , RUNNER_LIST(rmcast)
        , RUNNER_LIST(rnetmap)
    >;
    config = Config{};
    for (auto it = params.begin(); it != params.end(); ++it) {
        config.put(it->first, it->second);
    }

    auto netProt = config.getExt<std::string>("netProt");
    auto ipcCapacity = config.getExt<uint32_t>("ipcCapacity");
    if (vm.count("help")) {
        cout << desc << "\n";
        std::apply(
            [=](auto&&... args) {
                (args.help(netProt, ipcCapacity), ...);
            }
            , Runners()
        );
        return 0;
    }

    if (vm.count("cfg")) {
        auto cfg = config.getExt<std::string>("cfg");
        auto ifs = std::ifstream(cfg);
        cfgString = std::string((std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
    }

    SyncLogger::instance().setMinLogLevel((SyncLogger::Level)config.getExt<int>("logLevel"));

    HMBDC_LOG_N(" in effect config: ", config);

    config.put("ipcMaxMessageSizeRuntime", config.getExt<std::string>("bufWidth"));
    config.put("netMaxMessageSizeRuntime", config.getExt<std::string>("bufWidth"));

    config.put("ipcMessageQueueSizePower2Num", 15);
    std::apply(
        [&](auto&&... args) {
            (args(netProt, ipcCapacity, config), ...);
        }
        , Runners()
    );

    return 0;
}
