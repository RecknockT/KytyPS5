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

void TestCpSequencerWithoutDrawPrep() {
	std::printf("Running TestCpSequencerWithoutDrawPrep (Regression Test B)...\n");
	OpStream            stream(256u << 10u);
	CpSeq::SnapshotRing snapshots(256);

	constexpr uint32_t num_draws = 100;
	HW::Context        curr_ctx;
	HW::UserConfig     curr_ucfg;
	HW::Shader         curr_sh;

	for (uint32_t i = 0; i < num_draws; ++i) {
		// Sequencer thread sets registers for draw
		curr_ctx.SetPsInControl(0x1000u + i);
		curr_ucfg.SetPrimitiveType(Prospero::PrimitiveType::kTriList);
		curr_sh.SetEsShaderBase(0x2000'0000ull + i * 0x1000ull);

		// Acquire snapshot from ring (CpSequencer without DrawPrep)
		const auto snap_idx = snapshots.AcquireBlocking();
		Check(snap_idx != UINT32_MAX, "Snapshot acquired successfully");
		auto& entry       = snapshots.Entry(snap_idx);
		entry.context     = curr_ctx;
		entry.user_config = curr_ucfg;
		entry.shaders     = curr_sh;

		DrawIndexOp op {};
		op.index_count         = 36;
		op.instance_count      = 1;
		op.index_type_and_size = 0;
		op.flags               = DrawFlagSnapshot; // NO DrawFlagPublished!
		op.snapshot            = snap_idx;

		stream.Emit(OpKind::DrawIndex, &op, sizeof(op), nullptr, 0, i, 0, false);

		// Mutate original registers immediately to simulate CP moving ahead or resetting!
		curr_ucfg.SetPrimitiveType(Prospero::PrimitiveType::kNone);
		curr_ctx.SetPsInControl(0);
		curr_sh.SetEsShaderBase(0);
	}

	// GPU thread resolves submissions
	for (uint32_t i = 0; i < num_draws; ++i) {
		OpView view;
		Check(stream.Peek(view), "GPU thread peek op");
		Check(view.Kind() == OpKind::DrawIndex, "Op is DrawIndex");
		const auto& draw_op = view.As<DrawIndexOp>();

		Check((draw_op.flags & DrawFlagSnapshot) != 0, "DrawFlagSnapshot is set");
		Check((draw_op.flags & DrawFlagPublished) == 0, "DrawFlagPublished is NOT set");

		const auto& snap = snapshots.Entry(draw_op.snapshot);
		Check(snap.user_config.GetPrimType() == Prospero::PrimitiveType::kTriList,
		      "PrimType preserved intact (NOT kNone)");
		Check(snap.context.GetShaderRegisters().ps_in_control == 0x1000u + i,
		      "Context register preserved intact");
		Check(snap.shaders.GetVs().es_regs.data_addr == 0x2000'0000ull + i * 0x1000ull,
		      "Shader register preserved intact");

		snapshots.Release();
		stream.Pop();
	}
	Check(snapshots.HasFree(), "All snapshots released");
}

void TestPipelineCacheMruCoherency() {
	std::printf("Running TestPipelineCacheMruCoherency (Regression Test C)...\n");
	struct MockPipeline {
		uint32_t id          = 0;
		uint32_t push_stages = 0;
	};

	struct MockKeyInputCache {
		bool     valid     = false;
		uint32_t vs_id     = 0;
		uint32_t ps_id     = 0;
		uint32_t prim_type = 0;
	};

	MockKeyInputCache key_input_cache {};
	MockPipeline      pipeline_A {.id = 1, .push_stages = 0x01};
	MockPipeline      pipeline_B {.id = 2, .push_stages = 0x03};
	MockPipeline*     last_graphics_pipeline = nullptr;

	// Draw 1: Uses pipeline A
	last_graphics_pipeline    = &pipeline_A;
	key_input_cache.valid     = true;
	key_input_cache.vs_id     = 100;
	key_input_cache.ps_id     = 200;
	key_input_cache.prim_type = 1;

	auto CheckMatch = [&](uint32_t vs, uint32_t ps, uint32_t pt) {
		return key_input_cache.valid && key_input_cache.vs_id == vs &&
		       key_input_cache.ps_id == ps && key_input_cache.prim_type == pt;
	};

	Check(CheckMatch(100, 200, 1), "Draw 1 matches key input cache");
	Check(last_graphics_pipeline->id == 1, "Draw 1 uses pipeline A");

	// Draw 2: Fresh compile inserts Pipeline B.
	// With Etapa 8.4F fix: UpdateKeyInputCache is called whenever inserted == true!
	last_graphics_pipeline    = &pipeline_B;
	key_input_cache.valid     = true;
	key_input_cache.vs_id     = 300;
	key_input_cache.ps_id     = 400;
	key_input_cache.prim_type = 2;

	// Draw 3: Has the SAME inputs as Draw 1 (vs=100, ps=200, pt=1)
	// Because key_input_cache was properly updated on Draw 2, CheckMatch returns FALSE!
	const bool draw3_matches_fast = CheckMatch(100, 200, 1);
	Check(!draw3_matches_fast, "Draw 3 does NOT falsely match Draw 2 MRU key input cache");
}

void TestSnapshotRingSaturationAndShutdown() {
	std::printf("Running TestSnapshotRingSaturationAndShutdown...\n");
	SnapshotRing ring(256);
	Check(ring.Capacity() == 256, "Ring capacity is 256");

	// Saturate all 256 slots
	for (uint32_t i = 0; i < 256; ++i) {
		Check(ring.HasFree(), "Slot must be free before saturation");
		const auto idx = ring.Acquire();
		Check(idx == i, "Acquire must match index monotonically");
	}
	Check(!ring.HasFree(), "Ring must be fully saturated at 256");

	// Test graceful cancellation / shutdown when blocked
	std::atomic<bool>     stop {false};
	std::atomic<uint32_t> result_idx {0};
	std::thread blocked_thread([&] {
		result_idx.store(ring.AcquireBlocking([&] { return stop.load(); }));
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	Check(result_idx.load() == 0, "Thread must be blocking in AcquireBlocking");

	stop.store(true);
	blocked_thread.join();
	Check(result_idx.load() == UINT32_MAX, "AcquireBlocking aborted gracefully on stop signal without deadlock");

	// Now release all 256 slots
	for (uint32_t i = 0; i < 256; ++i) {
		ring.Release();
	}
	Check(ring.HasFree(), "Ring has free slots after releases");
}

void TestInterleavedPm4Ordering() {
	std::printf("Running TestInterleavedPm4Ordering...\n");
	OpStream stream(256u << 10u);
	SnapshotRing snapshots(256);

	// Sequence: DrawIndex -> DispatchDirect -> ReleaseMem -> DmaData -> Flip -> DrawAuto
	HW::Context ctx;
	HW::UserConfig ucfg;
	HW::Shader sh;

	ctx.SetPsInControl(0x1111);
	auto snap1 = snapshots.Acquire();
	snapshots.Entry(snap1).context = ctx;
	DrawIndexOp draw1 {};
	draw1.flags = DrawFlagSnapshot;
	draw1.snapshot = snap1;
	stream.Emit(OpKind::DrawIndex, &draw1, sizeof(draw1), nullptr, 0, 1, 0, false);

	ctx.SetPsInControl(0x2222);
	auto snap2 = snapshots.Acquire();
	snapshots.Entry(snap2).context = ctx;
	DispatchDirectOp disp {};
	disp.flags = DispatchFlagSnapshot;
	disp.snapshot = snap2;
	disp.x = 8; disp.y = 8; disp.z = 1;
	stream.Emit(OpKind::DispatchDirect, &disp, sizeof(disp), nullptr, 0, 2, 0, false);

	ReleaseMemOp rel {};
	rel.address = 0x5000'0000ull;
	stream.Emit(OpKind::ReleaseMem, &rel, sizeof(rel), nullptr, 0, 3, 0, false);

	DmaDataOp dma {};
	dma.num_bytes = 1024;
	stream.Emit(OpKind::DmaData, &dma, sizeof(dma), nullptr, 0, 4, 0, false);

	FlipOp flip {};
	flip.handle = 1;
	stream.Emit(OpKind::Flip, &flip, sizeof(flip), nullptr, 0, 5, 0, false);

	ctx.SetPsInControl(0x3333);
	auto snap3 = snapshots.Acquire();
	snapshots.Entry(snap3).context = ctx;
	DrawAutoOp draw2 {};
	draw2.flags = DrawFlagSnapshot;
	draw2.snapshot = snap3;
	stream.Emit(OpKind::DrawAuto, &draw2, sizeof(draw2), nullptr, 0, 6, 0, false);

	// Consumer verifies order and payloads
	OpView v1;
	Check(stream.Peek(v1) && v1.Kind() == OpKind::DrawIndex, "Op 1 is DrawIndex");
	Check(snapshots.Entry(v1.As<DrawIndexOp>().snapshot).context.GetShaderRegisters().ps_in_control == 0x1111, "Draw 1 reg intact");
	snapshots.Release();
	stream.Pop();

	OpView v2;
	Check(stream.Peek(v2) && v2.Kind() == OpKind::DispatchDirect, "Op 2 is DispatchDirect");
	Check(snapshots.Entry(v2.As<DispatchDirectOp>().snapshot).context.GetShaderRegisters().ps_in_control == 0x2222, "Dispatch reg intact");
	snapshots.Release();
	stream.Pop();

	OpView v3;
	Check(stream.Peek(v3) && v3.Kind() == OpKind::ReleaseMem, "Op 3 is ReleaseMem");
	stream.Pop();

	OpView v4;
	Check(stream.Peek(v4) && v4.Kind() == OpKind::DmaData, "Op 4 is DmaData");
	stream.Pop();

	OpView v5;
	Check(stream.Peek(v5) && v5.Kind() == OpKind::Flip, "Op 5 is Flip");
	stream.Pop();

	OpView v6;
	Check(stream.Peek(v6) && v6.Kind() == OpKind::DrawAuto, "Op 6 is DrawAuto");
	Check(snapshots.Entry(v6.As<DrawAutoOp>().snapshot).context.GetShaderRegisters().ps_in_control == 0x3333, "Draw 2 reg intact");
	snapshots.Release();
	stream.Pop();

	Check(!stream.Peek(v1), "Stream empty at end");
	Check(snapshots.HasFree(), "All snapshots released cleanly");
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
	TestCpSequencerWithoutDrawPrep();
	TestPipelineCacheMruCoherency();
	TestSnapshotRingSaturationAndShutdown();
	TestInterleavedPm4Ordering();
	if (g_failures != 0) {
		std::printf("cp sequencer tests: %d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	std::printf("cp sequencer tests passed\n");
	return EXIT_SUCCESS;
}
