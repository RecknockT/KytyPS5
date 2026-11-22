// Unit tests for CpSequencer, OpStream, and PM4 parallelization primitives.

#include "graphics/guest_gpu/command_processor/cpOps.h"
#include "graphics/guest_gpu/command_processor/cpSequencer.h"
#include "graphics/host_gpu/gpuTouchedPages.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace {

using namespace Libs::Graphics;
using namespace Libs::Graphics::CpSeq;

int g_failures = 0;

void Check(bool condition, const char* what) {
	if (!condition) {
		std::printf("FAILED: %s\n", what);
		++g_failures;
	}
}

constexpr uint32_t KindCount = static_cast<uint32_t>(OpKind::Count);

bool CarriesData(OpKind kind) {
	return kind == OpKind::WriteData || kind == OpKind::DumpConstRam;
}

struct Expected {
	OpKind               kind         = OpKind::Count;
	uint64_t             sequence     = 0;
	uint64_t             packet       = 0;
	uint64_t             packets_hash = 0;
	bool                 verify       = false;
	std::vector<uint8_t> payload;
	std::vector<uint8_t> data;
};

Expected MakeOp(std::mt19937_64& random, uint64_t sequence, uint32_t max_data) {
	Expected op;
	op.kind         = static_cast<OpKind>(random() % KindCount);
	op.sequence     = sequence;
	op.packet       = random();
	op.packets_hash = random();
	op.verify       = (random() & 1u) != 0;
	op.payload.resize(PayloadSize(op.kind));
	for (auto& byte: op.payload) {
		byte = static_cast<uint8_t>(random());
	}
	if (CarriesData(op.kind) && max_data != 0) {
		op.data.resize(random() % (max_data + 1u));
		for (auto& byte: op.data) {
			byte = static_cast<uint8_t>(random());
		}
	}
	return op;
}

void EmitOp(OpStream& stream, const Expected& op) {
	const auto sequence =
	    stream.Emit(op.kind, op.payload.data(), static_cast<uint32_t>(op.payload.size()),
	                op.data.empty() ? nullptr : op.data.data(),
	                static_cast<uint32_t>(op.data.size()), op.packet, op.packets_hash, op.verify);
	Check(sequence == op.sequence, "Emit returns the op's sequence number");
}

bool Matches(const OpView& view, const Expected& op) {
	const auto* header = view.header;
	bool        ok     = header->kind == op.kind && header->sequence == op.sequence &&
	          header->packet == op.packet && header->packets_hash == op.packets_hash &&
	          header->data_size == op.data.size() &&
	          ((header->flags & FlagVerify) != 0) == op.verify &&
	          header->ring.op == CommandStream::Op::Count &&
	          header->ring.size == RecordSize(static_cast<uint32_t>(op.payload.size()),
	                                          static_cast<uint32_t>(op.data.size()));
	ok = ok && std::memcmp(view.payload, op.payload.data(), op.payload.size()) == 0;
	ok = ok && (op.data.empty() || std::memcmp(view.data, op.data.data(), op.data.size()) == 0);
	if (op.verify) {
		ok = ok && header->verify_hash == HashOp(op.kind, view.payload,
		                                         static_cast<uint32_t>(op.payload.size()),
		                                         view.data,
		                                         static_cast<uint32_t>(op.data.size()));
	}
	return ok;
}

void TestTable() {
	for (uint32_t index = 0; index < KindCount; index++) {
		const auto kind = static_cast<OpKind>(index);
		Check(PayloadSize(kind) != 0 && PayloadSize(kind) % 8 == 0, "payload sizes");
		Check(std::strcmp(OpKindName(kind), "?") != 0, "op kind names");
	}
	Check(PayloadSize(OpKind::Count) == 0, "no payload for Count");
	Check(PayloadSize(OpKind::DrawIndirect) == sizeof(DrawIndirectOp) &&
	          PayloadSize(OpKind::DrawIndirectMulti) == sizeof(DrawIndirectOp) &&
	          PayloadSize(OpKind::WriteData) == sizeof(WriteDataOp) &&
	          PayloadSize(OpKind::ReadCheck) == sizeof(ReadCheckOp),
	      "payload size table");
	Check(IsLockstep(OpKind::WaitRegMem) && IsLockstep(OpKind::WaitFlipDone) &&
	          IsLockstep(OpKind::Predication) && IsLockstep(OpKind::CondExec) &&
	          IsLockstep(OpKind::Branch) && !IsLockstep(OpKind::DrawIndex) &&
	          !IsLockstep(OpKind::EndOfPipe) && !IsLockstep(OpKind::ReadCheck),
	      "lockstep kinds");
	Check(RecordSize(sizeof(WriteDataOp), 5) == sizeof(OpHeader) + sizeof(WriteDataOp) + 8,
	      "record sizes pad inline data to 8 bytes");

	const WriteDataOp write {0x1000, 2, 0};
	const std::array<uint32_t, 2> dwords {1, 2};
	const auto base = HashOp(OpKind::WriteData, &write, sizeof(write), dwords.data(), 8);
	auto       other = dwords;
	other[1]         = 3;
	Check(base != HashOp(OpKind::WriteData, &write, sizeof(write), other.data(), 8) &&
	          base != HashOp(OpKind::WriteData, &write, sizeof(write), dwords.data(), 4) &&
	          base != HashOp(OpKind::ReferenceClock, &write, sizeof(write), dwords.data(), 8),
	      "op hash covers kind, payload and data");
}

void TestRoundTrip() {
	OpStream        stream(512u << 10u);
	std::mt19937_64 random(1234);
	uint64_t        sequence = 0;
	for (uint32_t index = 0; index < KindCount * 64u; index++) {
		auto op = MakeOp(random, sequence++, 256);
		op.kind = static_cast<OpKind>(index % KindCount);
		op.payload.resize(PayloadSize(op.kind));
		if (!CarriesData(op.kind)) {
			op.data.clear();
		}
		EmitOp(stream, op);
		OpView view;
		Check(stream.Peek(view), "an emitted op is published");
		Check(Matches(view, op), "op round trip");
		stream.Pop();
		Check(!stream.Peek(view), "nothing after the consumed op");
	}

	Expected large;
	large.kind     = OpKind::WriteData;
	large.sequence = sequence++;
	large.verify   = true;
	large.payload.resize(sizeof(WriteDataOp));
	large.data.resize(16381u * 4u, 0x5a);
	Check(RecordSize(sizeof(WriteDataOp), 16381u * 4u) <= stream.MaxRecord(),
	      "the largest WRITE_DATA fits a record");
	EmitOp(stream, large);
	OpView view;
	Check(stream.Peek(view) && Matches(view, large), "largest op round trip");
	stream.Pop();
	Check(stream.Emitted() == sequence, "sequence numbers are consecutive");
}

void TestWrap() {
	OpStream        stream(64u << 10u);
	std::mt19937_64 random(99);
	std::vector<Expected> pending;
	uint64_t              sequence = 0;
	uint64_t              consumed = 0;
	const uint32_t        max_data = stream.MaxRecord() - static_cast<uint32_t>(sizeof(OpHeader)) -
	                          static_cast<uint32_t>(sizeof(WriteDataOp)) - 8u;
	for (uint32_t round = 0; round < 4000; round++) {
		const auto burst = 1u + static_cast<uint32_t>(random() % 2u);
		for (uint32_t i = 0; i < burst; i++) {
			auto op = MakeOp(random, sequence++, (random() % 8u) == 0 ? max_data : 512u);
			EmitOp(stream, op);
			pending.push_back(std::move(op));
		}
		while (!pending.empty()) {
			OpView view;
			if (!stream.Peek(view)) {
				Check(false, "a pending op is published");
				break;
			}
			Check(Matches(view, pending.front()), "op round trip across wraps");
			stream.Pop();
			pending.erase(pending.begin());
			consumed++;
		}
	}
	Check(consumed == sequence && stream.Bytes() > (64u << 10u) * 8u, "the ring wrapped");
}

void TestThreaded() {
	OpStream          stream(64u << 10u);
	constexpr uint64_t Count = 20000;
	std::thread       producer([&] {
        std::mt19937_64 random(7);
        for (uint64_t index = 0; index < Count; index++) {
            EmitOp(stream, MakeOp(random, index, 2048));
        }
    });
	std::mt19937_64 random(7);
	uint64_t        received = 0;
	bool            ok       = true;
	while (received < Count) {
		OpView view;
		if (!stream.Peek(view)) {
			std::this_thread::yield();
			continue;
		}
		ok = ok && Matches(view, MakeOp(random, received, 2048));
		stream.Pop();
		received++;
	}
	producer.join();
	Check(ok, "threaded ops arrive intact and in order");
}

void TestTouchedPages() {
	using GpuTouched::Pages;
	auto pages = std::make_unique<Pages>();
	constexpr uint64_t base = 0x0000000203000000ull;
	Check(!pages->AnyTouched(base, base + 0x100000), "a fresh bitmap is clean");
	pages->Mark(base + 0x1234, base + 0x1238);
	Check(pages->AnyTouched(base + 0x1000, base + 0x1001) &&
	          pages->AnyTouched(base + 0x1fff, base + 0x2000) &&
	          !pages->AnyTouched(base + 0x2000, base + 0x3000) &&
	          !pages->AnyTouched(base, base + 0x1000) &&
	          pages->AnyTouched(base, base + 0x100000),
	      "a mark covers its whole 4 KiB page and nothing else");

	constexpr uint64_t leaf = uint64_t {1} << GpuTouched::LeafShift;
	pages->Mark(4u * leaf - 0x10, 4u * leaf + 0x10);
	Check(pages->AnyTouched(4u * leaf - 1u, 4u * leaf) &&
	          pages->AnyTouched(4u * leaf, 4u * leaf + 1u) &&
	          !pages->AnyTouched(4u * leaf + 0x1000, 4u * leaf + 0x2000) &&
	          !pages->AnyTouched(4u * leaf - 0x2000, 4u * leaf - 0x1000),
	      "marks across a leaf boundary");

	pages->Mark(base + 0x400000, base + 0x400000 + 257u * 0x1000u);
	bool all = true;
	for (uint64_t page = 0; page < 257u; page++) {
		const auto address = base + 0x400000 + page * 0x1000u;
		all = all && pages->AnyTouched(address, address + 1u);
	}
	Check(all && !pages->AnyTouched(base + 0x400000 + 257u * 0x1000u,
	                                base + 0x400000 + 258u * 0x1000u),
	      "a multi-word range marks exactly its pages");
	Check(!pages->Universe() && !pages->AnyTouched(0x7000000000ull, 0x7000001000ull),
	      "no universe yet");
	pages->Mark(0, UINT64_MAX);
	Check(pages->Universe() && pages->AnyTouched(0x7000000000ull, 0x7000001000ull),
	      "an unknown range makes every range touched");
	pages->ResetForTest();
	Check(!pages->Universe() && !pages->AnyTouched(base, base + 0x1000000),
	      "reset for the next check");

	auto fine = std::make_unique<GpuTouched::PagesT<10>>();
	fine->Mark(base + 0x1234, base + 0x1238);
	Check(fine->AnyTouched(base + 0x1000, base + 0x1001) && fine->AnyTouched(base + 0x13ff, base + 0x1400) &&
	          !fine->AnyTouched(base + 0x1400, base + 0x2000) && !fine->AnyTouched(base + 0x0c00, base + 0x1000),
	      "a fine mark covers its whole 1 KiB block and nothing else");
	fine->Mark(base + 0x10000, base + 0x10000 + 65u * 0x400u);
	Check(fine->AnyTouched(base + 0x10000 + 64u * 0x400u, base + 0x10000 + 65u * 0x400u) &&
	          !fine->AnyTouched(base + 0x10000 + 65u * 0x400u, base + 0x10000 + 66u * 0x400u),
	      "a fine multi-word range marks exactly its blocks");

	std::atomic<uint64_t> published {0};
	std::atomic<bool>     seen_all {true};
	constexpr uint64_t    count = 20000;
	std::thread           marker([&] {
        for (uint64_t page = 0; page < count; page++) {
            const auto address = base + page * 0x1000u;
            pages->Mark(address, address + 8u);
            published.store(page + 1u, std::memory_order_seq_cst);
        }
    });
	for (uint64_t checked = 0; checked < count;) {
		const auto limit = published.load(std::memory_order_seq_cst);
		for (; checked < limit; checked++) {
			const auto address = base + checked * 0x1000u;
			if (!pages->AnyTouched(address, address + 0x1000u)) {
				seen_all.store(false);
			}
		}
	}
	marker.join();
	Check(seen_all.load(), "a mark finished before a check is seen by it");
}

void TestSnapshotRing() {
	CpSeq::SnapshotRing ring(4);
	std::array<uint32_t, 4> taken {};
	for (auto& index: taken) {
		Check(ring.HasFree(), "free entries while fewer than capacity are in use");
		index = ring.Acquire();
	}
	Check(!ring.HasFree(), "full at capacity");
	Check(taken == std::array<uint32_t, 4> {0, 1, 2, 3}, "entries in order");
	ring.Release();
	Check(ring.HasFree() && ring.Acquire() == 0, "a released entry is reused in order");
	Check(!ring.HasFree(), "full again");

	constexpr uint32_t    count = 5000;
	std::atomic<uint32_t> produced {0};
	CpSeq::SnapshotRing   shared(8);
	std::thread           producer([&] {
        for (uint32_t i = 0; i < count; i++) {
            while (!shared.HasFree()) {
                std::this_thread::yield();
            }
            const auto index = shared.Acquire();
            shared.Entry(index).context.SetPsInControl(i);
            produced.store(i + 1u, std::memory_order_release);
        }
    });
	bool ordered = true;
	for (uint32_t i = 0; i < count; i++) {
		while (produced.load(std::memory_order_acquire) <= i) {
			std::this_thread::yield();
		}
		ordered = ordered && shared.Entry(i % 8u).context.GetShaderRegisters().ps_in_control == i;
		shared.Release();
	}
	producer.join();
	Check(ordered, "snapshots arrive intact and in order");
}

void TestCommandOrderingAndIndirect() {
	OpStream stream(256u << 10u);

	DrawIndirectOp indirect_op {};
	indirect_op.args_base           = 0x10000000ull;
	indirect_op.index_base_addr     = 0x20000000ull;
	indirect_op.count_addr          = 0x30000000ull;
	indirect_op.data_offset         = 64;
	indirect_op.max_count_or_count  = 16;
	indirect_op.stride              = 32;
	indirect_op.flags               = IndirectFlagIndexed | IndirectFlagSnapshot;
	indirect_op.snapshot            = 5;

	const auto seq0 = stream.Emit(OpKind::DrawIndirectMulti, &indirect_op, sizeof(indirect_op),
	                              nullptr, 0, 1, 0x1111, false);

	DispatchDirectOp dispatch_op {};
	dispatch_op.x        = 32;
	dispatch_op.y        = 16;
	dispatch_op.z        = 2;
	dispatch_op.mode     = 0x40;
	dispatch_op.flags    = DispatchFlagSnapshot;
	dispatch_op.snapshot = 6;

	const auto seq1 = stream.Emit(OpKind::DispatchDirect, &dispatch_op, sizeof(dispatch_op),
	                              nullptr, 0, 2, 0x2222, false);

	WaitRegMemOp wait_op {};
	wait_op.addr    = 0x40000000ull;
	wait_op.ref     = 1;
	wait_op.mask    = 1;
	wait_op.func    = 3; // Equal
	wait_op.wait_op = 0;
	wait_op.size    = 4;

	const auto seq2 = stream.Emit(OpKind::WaitRegMem, &wait_op, sizeof(wait_op),
	                              nullptr, 0, 3, 0x3333, false);

	Check(seq0 == 0 && seq1 == 1 && seq2 == 2, "sequential op ordering preserved");

	OpView v0;
	Check(stream.Peek(v0), "peek op 0");
	Check(v0.Kind() == OpKind::DrawIndirectMulti, "op 0 is indirect draw multi");
	const auto& out_indirect = v0.As<DrawIndirectOp>();
	Check(out_indirect.args_base == 0x10000000ull && out_indirect.max_count_or_count == 16,
	      "indirect draw payload preserved");
	stream.Pop();

	OpView v1;
	Check(stream.Peek(v1), "peek op 1");
	Check(v1.Kind() == OpKind::DispatchDirect, "op 1 is dispatch direct");
	const auto& out_disp = v1.As<DispatchDirectOp>();
	Check(out_disp.x == 32 && out_disp.y == 16 && out_disp.z == 2, "dispatch dimensions preserved");
	stream.Pop();

	OpView v2;
	Check(stream.Peek(v2), "peek op 2");
	Check(v2.Kind() == OpKind::WaitRegMem && IsLockstep(v2.Kind()), "op 2 is lockstep wait");
	const auto& out_wait = v2.As<WaitRegMemOp>();
	Check(out_wait.addr == 0x40000000ull && out_wait.ref == 1, "wait reg mem payload preserved");
	stream.Pop();

	Check(!stream.Peek(v0), "stream is empty after pop");
}

} // namespace

int main() {
	TestTable();
	TestRoundTrip();
	TestWrap();
	TestThreaded();
	TestTouchedPages();
	TestSnapshotRing();
	TestCommandOrderingAndIndirect();
	if (g_failures != 0) {
		std::printf("cp sequencer tests: %d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	std::printf("cp sequencer tests passed\n");
	return EXIT_SUCCESS;
}
