#include <cstdio>

#include "ipc_object_stub.h"
#include "message_option.h"
#include "message_parcel.h"
#include "system_ability.h"

namespace OHOS {
constexpr int32_t ARKVM_TEST_SA_ID = 9000;

class TestStub : public IPCObjectStub {
public:
    TestStub() : IPCObjectStub(u"arkvm.test") {}

    int OnRemoteRequest(uint32_t code, MessageParcel &data, MessageParcel &reply,
                        MessageOption &option) override
    {
        std::string payload = data.ReadString();
        printf("sa: code=%u payload=%s\n", code, payload.c_str());
        reply.WriteString("pong");
        return 0;
    }
};

class ArkvmTestSa : public SystemAbility {
    DECLARE_SYSTEM_ABILITY(ArkvmTestSa);

public:
    ArkvmTestSa(int32_t saId, bool runOnCreate) : SystemAbility(saId, runOnCreate) {}
    ~ArkvmTestSa() override = default;

protected:
    void OnStart() override
    {
        printf("sa: OnStart\n");
        bool ok = Publish(new TestStub());
        printf("sa: Publish %s\n", ok ? "ok" : "failed");
    }

    void OnStop() override
    {
        printf("sa: OnStop\n");
    }
};

REGISTER_SYSTEM_ABILITY_BY_ID(ArkvmTestSa, ARKVM_TEST_SA_ID, true)
} // namespace OHOS
