#include "alchemy/darwin/launchd/mach_services.hpp"

#include "alchemy/darwin/libsystem/mach/mach_ipc.hpp"

#include <map>
#include <mutex>

// A Mach port space with no guest code behind it: the session launchd holds
// its jobs' service ports here, other processes are granted send rights to
// them, messages queue here until the job checks in, and then the receive
// right (with the queue) moves to the job (MachIpc::offer_receive).
namespace alchemy::launchd {
namespace {

using namespace libsystem::mach;

class Services final : public MachServices {
public:
    Services() {
        MachIpc::Guest guest;
        guest.allocate = [](std::uint64_t) -> Address { return 0; };
        guest.deallocate = [](Address, std::uint64_t) {};
        guest.kernel_name = [](Name) { return false; };
        guest.interrupted = [] { return false; };
        guest.audit_token = [] { return std::array<std::uint32_t, 8>{}; };
        guest.serialize_object = [](const std::shared_ptr<KernelObject>&, std::uint32_t) -> std::optional<std::vector<std::uint8_t>> {
            return std::nullopt;
        };
        guest.deserialize_object = [](const std::vector<std::uint8_t>& data) -> std::shared_ptr<KernelObject> {
            discard_wire_object(data);
            return nullptr;
        };
        guest.forward_object = &forward_wire_object;
        guest.discard_object = &discard_wire_object;
        guest.receive_ool = [](const std::vector<std::uint8_t>& object, std::uint64_t) -> Address {
            discard_wire_object(object);
            return 0;
        };
        guest.task_call = [](std::uint32_t, const std::vector<std::uint8_t>&) { return std::vector<std::uint8_t>{}; };
        ipc_ = std::make_shared<MachIpc>(std::move(guest));
        transport_ = Transport::create(std::weak_ptr<Transport::Handler>(ipc_));
        ipc_->connect(transport_, transport_->self());
        static_cast<void>(transport_->start());
    }
    ~Services() override { transport_->stop(); }

    std::uint32_t self() const override { return transport_->self(); }

    bool create(const std::string& name) override {
        Name receive{};
        if (ipc_->allocate(right::receive, receive) != kern::success) return false;
        const auto key = ipc_->export_receive(receive);
        if (!key.pid) { static_cast<void>(ipc_->mod_refs(receive, right::receive, -1)); return false; }
        std::lock_guard lock(mutex_);
        held_[name] = Held{receive, key, 0};
        return true;
    }

    void destroy(const std::string& name) override {
        Held held;
        {
            std::lock_guard lock(mutex_);
            const auto found = held_.find(name);
            if (found == held_.end()) return;
            held = found->second;
            held_.erase(found);
        }
        // Moved to a job: it dies with the job's process.
        if (!held.moved_to) static_cast<void>(ipc_->mod_refs(held.name, right::receive, -1));
    }

    std::optional<ServiceLocation> locate(const std::string& name) override {
        std::lock_guard lock(mutex_);
        const auto found = held_.find(name);
        if (found == held_.end()) return std::nullopt;
        const auto& held = found->second;
        return ServiceLocation{held.moved_to ? held.moved_to : self(), held.key.pid, held.key.id};
    }

    bool offer(const std::string& name, std::uint32_t pid) override {
        std::lock_guard lock(mutex_);
        const auto found = held_.find(name);
        if (found == held_.end() || found->second.moved_to) return false;
        if (ipc_->offer_receive(pid, name, found->second.name) != kern::success) return false;
        found->second.moved_to = pid;
        return true;
    }

    std::vector<std::string> pending() override {
        std::vector<std::pair<std::string, Name>> names;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [name, held] : held_)
                if (!held.moved_to) names.emplace_back(name, held.name);
        }
        std::vector<std::string> out;
        for (const auto& [name, receive] : names) {
            std::uint32_t seqno = 0;
            MachIpc::PeekResult result;
            if (ipc_->peek(receive, seqno, result) == kern::success) out.push_back(name);
        }
        return out;
    }

    std::uint64_t epoch() override { return ipc_->epoch(); }
    void wait(std::uint64_t seen, std::chrono::milliseconds limit) override { ipc_->wait(seen, limit); }

private:
    struct Held {
        Name name{};
        Key key{};
        std::uint32_t moved_to{};   // the job the receive right went to (0: held here)
    };
    std::shared_ptr<MachIpc> ipc_;
    std::shared_ptr<Transport> transport_;
    std::mutex mutex_;
    std::map<std::string, Held> held_;
};

} // namespace

std::unique_ptr<MachServices> make_mach_services() { return std::make_unique<Services>(); }

} // namespace alchemy::launchd
