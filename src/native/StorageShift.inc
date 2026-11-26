// The caller has proved 128 < n <= 256 KiB and checked OS support for YMM state.
// Snapshot both ends, then copy whole aligned destination vectors between them.
// Traversing away from the overlap keeps every interior load ahead of the store
// that could destroy it. The saved ends cover even a one-byte misalignment.
void emit_medium_storage_shift(std::ostream& out, bool right) {
    out << "define internal fastcc void @bolt.shift." << (right ? "right" : "left")
        << "(ptr %dst, ptr %src, i64 %n) noinline nounwind \"target-features\"=\"+avx2\" {\n"
        << R"(entry:
  %last = sub i64 %n, 32
  %src.last = getelementptr i8, ptr %src, i64 %last
  %dst.last = getelementptr i8, ptr %dst, i64 %last
  %head = load <4 x i64>, ptr %src, align 1
  %tail = load <4 x i64>, ptr %src.last, align 1
  %addr = ptrtoint ptr %dst to i64
  %misalignment = and i64 %addr, 31
  %start = sub i64 32, %misalignment
  %distance = sub i64 %n, %start
  %exclusive = sub i64 %distance, 1
  %blocks = lshr i64 %exclusive, 5
  %bytes = shl i64 %blocks, 5
  %end = add i64 %start, %bytes
  br label %groups
groups:
  %pos = phi i64 [ )" << (right ? "%end" : "%start") << R"(, %entry ], [ %after4, %four ]
  %remaining = phi i64 [ %blocks, %entry ], [ %less4, %four ]
  %group = icmp uge i64 %remaining, 4
  br i1 %group, label %four, label %remainder
four:
)";
    auto copy = [&](int vectors, const char* pos, const char* next) {
        for (int i=0; i<vectors; ++i) {
            const auto id=std::string(next)+std::to_string(i);
            const int displacement=right ? -(i+1)*32 : i*32;
            out << "  %offset." << id << " = add i64 %" << pos << ", " << displacement << "\n"
                << "  %from." << id << " = getelementptr i8, ptr %src, i64 %offset." << id << "\n"
                << "  %to." << id << " = getelementptr i8, ptr %dst, i64 %offset." << id << "\n"
                << "  %v." << id << " = load <4 x i64>, ptr %from." << id << ", align 1\n"
                << "  store <4 x i64> %v." << id << ", ptr %to." << id << ", align 32\n";
        }
        if (vectors>1)
            out << "  %" << next << " = add i64 %" << pos << ", " << (right ? -32 : 32)*vectors << "\n";
    };
    copy(4,"pos","after4");
    out << R"(  %less4 = sub i64 %remaining, 4
  br label %groups
remainder:
  %pair = icmp uge i64 %remaining, 2
  br i1 %pair, label %two, label %single
two:
)";
    copy(2,"pos","after2");
    out << R"(  br label %single
single:
  %finalpos = phi i64 [ %pos, %remainder ], [ %after2, %two ]
  %odd = trunc i64 %remaining to i1
  br i1 %odd, label %one, label %finish
one:
)";
    copy(1,"finalpos","lastblock");
    out << R"(  br label %finish
finish:
  store <4 x i64> %head, ptr %dst, align 1
  store <4 x i64> %tail, ptr %dst.last, align 1
  ret void
}
)";
}

// A shift by one dword or qword lets adjacent aligned cache lines supply a
// whole destination line. Carry the previous load instead of reloading it at
// an unaligned address. The caller checks the displacement and n >= 4096.
void emit_large_storage_shift(std::ostream& out, unsigned stride, bool right) {
    const int step = right ? -64 : 64;
    out << "define internal fastcc void @bolt.bulk." << (right ? "right." : "left.") << stride
        << "(ptr %dst, i64 %n) noinline nounwind \"target-features\"=\"+avx512f\" {\nentry:\n"
        << "  %src = getelementptr i8, ptr %dst, i64 " << (right ? -int(stride) : int(stride)) << "\n";
    // Leave at least one complete line of source on each side for the aligned
    // loads. The saved 128-byte ends cover the remaining partial lines.
    for (int i=0; i<4; ++i) {
        const auto offset=i<2 ? std::to_string(i*64) : "%off"+std::to_string(i);
        if (i>=2) out << "  " << offset << " = sub i64 %n, " << (i-1)*64 << "\n";
        out << "  %s" << i << " = getelementptr i8, ptr %src, i64 " << offset << "\n"
            << "  %d" << i << " = getelementptr i8, ptr %dst, i64 " << offset << "\n"
            << "  %edge" << i << " = load <16 x i32>, ptr %s" << i << ", align 1\n";
    }
    out << R"(  %d = ptrtoint ptr %dst to i64
  %a = add i64 %d, 128
  %b = add i64 %d, %n
  %c = sub i64 %b, 64
  %start = and i64 %a, -64
  %end = and i64 %c, -64
)" << "  %initial = add i64 " << (right ? "%end, -64" : "%start, 0") << "\n"
        << "  %p0 = inttoptr i64 %initial to ptr\n"
        << "  %carry0 = load <16 x i32>, ptr %p0, align 64\n  br label %loop\nloop:\n"
        << "  %at = phi i64 [ %initial, %entry ], [ %next, %loop ]\n"
        << "  %carry = phi <16 x i32> [ %carry0, %entry ], [ %other, %loop ]\n"
        << "  %next = add i64 %at, " << step << "\n"
        << "  %p = inttoptr i64 %at to ptr\n  %q = inttoptr i64 %next to ptr\n"
        << "  %other = load <16 x i32>, ptr %q, align 64\n"
        << "  %shifted = shufflevector <16 x i32> " << (right ? "%other, <16 x i32> %carry" : "%carry, <16 x i32> %other")
        << ", <16 x i32> <";
    const auto lane=right ? 16-stride/4 : stride/4;
    for (unsigned i=0; i<16; ++i) out << (i ? ", " : "") << "i32 " << lane+i;
    out << ">\n  store <16 x i32> %shifted, ptr %p, align 64\n"
        << "  %done = icmp eq i64 " << (right ? "%at, %start" : "%next, %end") << "\n"
        << "  br i1 %done, label %finish, label %loop\nfinish:\n";
    for (int i=0; i<4; ++i) out << "  store <16 x i32> %edge" << i << ", ptr %d" << i << ", align 1\n";
    out << "  ret void\n}\n";
}
