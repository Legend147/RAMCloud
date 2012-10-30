/* Copyright (c) 2010-2012 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "TestUtil.h"

#include "Segment.h"
#include "StringUtil.h"
#include "Log.h"
#include "LogEntryTypes.h"
#include "ServerConfig.h"

namespace RAMCloud {

struct SegmentAndSegletSize {
    uint32_t segmentSize;
    uint32_t segletSize;
    uint32_t getSegletsPerSegment() { return segmentSize / segletSize; }
};

class SegmentAndAllocator {
  public:
    explicit SegmentAndAllocator(SegmentAndSegletSize* segmentAndSegletSize)
        : segmentSize(segmentAndSegletSize->segmentSize),
          segletSize(segmentAndSegletSize->segletSize),
          allocator(),
          segment()
    {
        ServerConfig serverConfig(ServerConfig::forTesting());
        serverConfig.segmentSize = segmentSize;
        serverConfig.segletSize = segletSize;
        allocator.construct(serverConfig);

        vector<Seglet*> seglets;
        EXPECT_TRUE(allocator->alloc(SegletAllocator::DEFAULT,
                    segmentSize / segletSize,
                    seglets));
        segment.construct(seglets, segletSize);
    }

    const uint32_t segmentSize;
    const uint32_t segletSize;
    Tub<SegletAllocator> allocator;
    Tub<Segment> segment;

    DISALLOW_COPY_AND_ASSIGN(SegmentAndAllocator);
};

/**
 * Unit tests for Segment.
 */
class SegmentTest : public ::testing::TestWithParam<SegmentAndSegletSize*> {
  public:
    SegmentTest()
    {
    }

    DISALLOW_COPY_AND_ASSIGN(SegmentTest);
};

// Run tests with various different seglet sizes to stress the code with
// different fragmentation in the backing segment memory.

SegmentAndSegletSize boringDefault = {
    Segment::DEFAULT_SEGMENT_SIZE,
    Seglet::DEFAULT_SEGLET_SIZE
};

SegmentAndSegletSize extraFragmented = {
    66560,
    256
};

INSTANTIATE_TEST_CASE_P(SegmentTestAllocations,
                        SegmentTest,
                        ::testing::Values(&boringDefault,
                                          &extraFragmented));

TEST_P(SegmentTest, constructor) {
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;

    EXPECT_FALSE(s.closed);
    EXPECT_EQ(0U, s.head);
}

TEST_F(SegmentTest, constructor_priorSegmentBuffer) {
    Segment previous;
    previous.append(LOG_ENTRY_TYPE_OBJ, "hi", 3);
    Buffer buffer;
    previous.appendToBuffer(buffer);

    const void* p = buffer.getRange(0, buffer.getTotalLength());
    Segment s(p, buffer.getTotalLength());

    EXPECT_EQ(0U, s.seglets.size());
    EXPECT_EQ(1U, s.segletBlocks.size());
    EXPECT_TRUE(s.closed);
    EXPECT_EQ(s.head, buffer.getTotalLength());
    EXPECT_EQ(p, s.segletBlocks[0]);
    EXPECT_FALSE(s.mustFreeBlocks);
}

TEST_P(SegmentTest, append_blackBox) {
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;

    char buf[1000];
    for (uint32_t i = 0; i < 1000; i += 100) {
        uint32_t offset;
        EXPECT_TRUE(s.append(LOG_ENTRY_TYPE_OBJ, buf, i, &offset));

        Buffer buffer;
        s.getEntry(offset, buffer);
        EXPECT_EQ(i, buffer.getTotalLength());
        EXPECT_EQ(0, memcmp(buf, buffer.getRange(0, i), i));
    }
}

TEST_P(SegmentTest, append_outOfSpace) {
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;

    // How many N-length writes can we make to this segment?
    char buf[107];
    uint32_t bytesPerAppend = sizeof(buf) + 2;  // EntryHeader, length, data
    uint32_t expectedAppends = GetParam()->segmentSize / bytesPerAppend;

    uint32_t actualAppends = 0;
    while (s.append(LOG_ENTRY_TYPE_OBJ, buf, sizeof(buf)))
        actualAppends++;

    EXPECT_EQ(expectedAppends, actualAppends);
    EXPECT_EQ(GetParam()->getSegletsPerSegment(), s.getSegletsAllocated());
}

TEST_P(SegmentTest, append_whiteBox) {
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;

    uint32_t offset;
    s.append(LOG_ENTRY_TYPE_OBJ, "hi", 2, &offset);

    EXPECT_EQ(0U, offset);
    Segment::Certificate certificate;
    EXPECT_EQ(4U, s.getAppendedLength(&certificate));
    EXPECT_EQ(4u, certificate.segmentLength);
    EXPECT_EQ(0x87a632e2u, certificate.checksum);

    Buffer buffer;
    s.appendToBuffer(buffer);
    EXPECT_EQ(0, memcmp("hi", buffer.getRange(2, 2), 2));
}

TEST_P(SegmentTest, append_differentLengthBytes) {
    uint32_t oneByteLengths[] = { 0, 255 };
    uint32_t twoByteLengths[] = { 256, 65535 };
    uint32_t threeByteLengths[] = { 65536 };
    struct {
        uint32_t expectedLengthBytes;
        uint32_t* bytesToAppend;
        uint32_t bytesToAppendLength;
    } tests[] = {
        { 1, oneByteLengths, arrayLength(oneByteLengths) },
        { 2, twoByteLengths, arrayLength(twoByteLengths) },
        { 3, threeByteLengths, arrayLength(threeByteLengths) }
        // 4-byte lengths? Fuhgeddaboudit!
    };

    for (uint32_t i = 0; i < unsafeArrayLength(tests); i++) {
        for (uint32_t j = 0; j < tests[i].bytesToAppendLength; j++) {
            uint32_t length = tests[i].bytesToAppend[j];

            char buf[length];
            SegmentAndAllocator segAndAlloc(GetParam());
            Segment& s = *segAndAlloc.segment;
            s.append(LOG_ENTRY_TYPE_OBJ, buf, length);
            EXPECT_EQ(sizeof(Segment::EntryHeader) +
                        tests[i].expectedLengthBytes + length,
                      s.getAppendedLength());

            const Segment::EntryHeader* entryHeader = NULL;
            Buffer buffer;
            s.appendToBuffer(buffer, 0, sizeof(*entryHeader));
            entryHeader = buffer.getStart<Segment::EntryHeader>();

            EXPECT_EQ(LOG_ENTRY_TYPE_OBJ, entryHeader->getType());
            EXPECT_EQ(tests[i].expectedLengthBytes,
                      entryHeader->getLengthBytes());
        }
    }
}

TEST_P(SegmentTest, close) {
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;
    EXPECT_FALSE(s.closed);
    s.close();
    EXPECT_TRUE(s.closed);
}

TEST_P(SegmentTest, appendToBuffer_partial) {
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;
    s.append(LOG_ENTRY_TYPE_OBJ, "this is only a test!", 21);

    Buffer buffer;
    s.appendToBuffer(buffer, 2, 21);
    EXPECT_EQ(21U, buffer.getTotalLength());
    EXPECT_STREQ("this is only a test!",
        reinterpret_cast<const char*>(buffer.getRange(0, 21)));
}

TEST_P(SegmentTest, appendToBuffer_all) {
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;

    Buffer buffer;
    s.appendToBuffer(buffer);
    EXPECT_EQ(0U, buffer.getTotalLength());

    buffer.reset();
    s.append(LOG_ENTRY_TYPE_OBJ, "yo!", 3);
    s.appendToBuffer(buffer);
    EXPECT_EQ(5U, buffer.getTotalLength());
}

TEST_P(SegmentTest, getEntry) {
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;
    uint32_t offset;
    s.append(LOG_ENTRY_TYPE_OBJ, "this is only a test!", 21, &offset);

    Buffer buffer;
    EXPECT_EQ(LOG_ENTRY_TYPE_OBJ, s.getEntry(offset, buffer));
    EXPECT_EQ(21U, buffer.getTotalLength());
    EXPECT_STREQ("this is only a test!",
        reinterpret_cast<const char*>(buffer.getRange(0, 21)));
}

TEST_P(SegmentTest, getAppendedLength) {
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;

    Segment::Certificate certificate;
    EXPECT_EQ(0lu, s.getAppendedLength(&certificate));
    EXPECT_EQ(0lu, certificate.segmentLength);
    EXPECT_EQ(0x48674bc7lu, certificate.checksum);
    s.append(LOG_ENTRY_TYPE_OBJ, "yo!", 3);
    EXPECT_EQ(5lu, s.getAppendedLength(&certificate));
    EXPECT_EQ(5lu, certificate.segmentLength);
    EXPECT_EQ(0x62f2f7f6u, certificate.checksum);
}

TEST_P(SegmentTest, getSegletsAllocated) {
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;
    EXPECT_EQ(segAndAlloc.segmentSize / segAndAlloc.segletSize,
              s.getSegletsAllocated());
}

TEST_P(SegmentTest, getSegletsInUse) {
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;
    EXPECT_EQ(0U, s.getSegletsInUse());

    char buf[segAndAlloc.segletSize];
    bool ok = s.append(LOG_ENTRY_TYPE_OBJ, buf, segAndAlloc.segletSize);
    if ((segAndAlloc.segmentSize / segAndAlloc.segletSize) > 1) {
        EXPECT_TRUE(ok);
        EXPECT_GE(s.getSegletsInUse(), 2U);
        EXPECT_LE(s.getSegletsInUse(), 3U);
    } else {
        EXPECT_FALSE(ok);
        EXPECT_EQ(0U, s.getSegletsInUse());
    }
}

TEST_P(SegmentTest, peek) {
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;
    const void* pointer = NULL;
    void* const nullPtr = NULL;

    EXPECT_EQ(1U, s.peek(GetParam()->segmentSize - 1, &pointer));
    EXPECT_EQ(0U, s.peek(GetParam()->segmentSize, &pointer));
    EXPECT_EQ(0U, s.peek(GetParam()->segmentSize + 1, &pointer));
    EXPECT_EQ(GetParam()->segletSize, s.peek(0, &pointer));
    EXPECT_EQ(GetParam()->segletSize - 1, s.peek(1, &pointer));

    pointer = NULL;
    EXPECT_NE(0U, s.peek(GetParam()->segmentSize - 1, &pointer));
    EXPECT_NE(nullPtr, pointer);

    pointer = NULL;
    EXPECT_EQ(0U, s.peek(GetParam()->segmentSize, &pointer));
    EXPECT_EQ(nullPtr, pointer);

    pointer = NULL;
    EXPECT_EQ(0U, s.peek(GetParam()->segmentSize + 1, &pointer));
    EXPECT_EQ(nullPtr, pointer);

    pointer = NULL;
    EXPECT_EQ(GetParam()->segletSize, s.peek(0, &pointer));
    EXPECT_EQ(s.segletBlocks[0], pointer);
}

TEST_P(SegmentTest, hasSpaceFor) {
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;

    EXPECT_TRUE(s.hasSpaceFor(NULL, 0));

    uint32_t lengths[2];

    s.closed = true;
    lengths[0] = 0;
    EXPECT_FALSE(s.hasSpaceFor(lengths, 1));

    s.closed = false;
    EXPECT_TRUE(s.hasSpaceFor(lengths, 1));

    uint32_t totalFreeBytes = s.getSegletsAllocated() * s.segletSize - s.head;
    lengths[0] = totalFreeBytes;
    EXPECT_FALSE(s.hasSpaceFor(lengths, 1));

    lengths[0] = totalFreeBytes - 4;        // EntryHeader + 3 bytes length
    EXPECT_TRUE(s.hasSpaceFor(lengths, 1));

    lengths[1] = 3;
    EXPECT_FALSE(s.hasSpaceFor(lengths, 2));

    lengths[0] = lengths[1] = lengths[2] = 20;
    lengths[3] = 999999999;
    EXPECT_TRUE(s.hasSpaceFor(lengths, 3));
}

TEST_P(SegmentTest, copyOut) {
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;
    uint32_t segmentSize = GetParam()->segmentSize;

    char buf[1024];
    EXPECT_EQ(0U, s.copyOut(segmentSize, buf, sizeof(buf)));
    EXPECT_EQ(5U, s.copyOut(segmentSize - 5, buf, sizeof(buf)));
    EXPECT_EQ(sizeof32(buf),
              s.copyOut(segmentSize - sizeof32(buf), buf, sizeof(buf)));

    char src[100];
    s.copyIn(5, src, sizeof(src));
    s.copyOut(5, buf, sizeof(src));
    EXPECT_EQ(0, memcmp(src, buf, sizeof(src)));
}

TEST_P(SegmentTest, copyIn) {
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;
    uint32_t segmentSize = GetParam()->segmentSize;

    char buf[1024];
    EXPECT_EQ(0U, s.copyIn(segmentSize, buf, sizeof(buf)));
    EXPECT_EQ(5U, s.copyIn(segmentSize - 5, buf, sizeof(buf)));
    EXPECT_EQ(sizeof32(buf),
              s.copyIn(segmentSize - sizeof32(buf), buf, sizeof(buf)));

    // SegmentTest_copyOut tests that correct data is copied in and out.
}

TEST_P(SegmentTest, copyInFromBuffer) {
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;
    uint32_t segmentSize = GetParam()->segmentSize;

    char buf[1024];
    Buffer buffer;
    buffer.append(buf, sizeof(buf));

    EXPECT_EQ(0U, s.copyInFromBuffer(segmentSize, buffer, 0, sizeof(buf)));
    EXPECT_EQ(5U, s.copyInFromBuffer(segmentSize - 5, buffer, 0, sizeof(buf)));
    EXPECT_EQ(sizeof32(buf),
       s.copyInFromBuffer(segmentSize - sizeof32(buf), buffer, 0, sizeof(buf)));

    char buf2[1024];

    s.copyInFromBuffer(6, buffer, 0, sizeof(buf));
    s.copyOut(6, buf2, sizeof(buf));
    EXPECT_EQ(0, memcmp(buf, buf2, sizeof(buf)));

    EXPECT_EQ(83U, s.copyInFromBuffer(12, buffer, 0, 83));
    s.copyOut(12, buf2, 83);
    EXPECT_EQ(0, memcmp(&buf[0], buf2, 83));

    EXPECT_EQ(28U, s.copyInFromBuffer(19, buffer, 2, 28));
    s.copyOut(19, buf2, 28);
    EXPECT_EQ(0, memcmp(&buf[2], buf2, 28));
}

TEST_P(SegmentTest, checkMetadataIntegrity_simple) {
    TestLog::Enable _;
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;

    Segment::Certificate certificate;
    s.getAppendedLength(&certificate);
    EXPECT_TRUE(s.checkMetadataIntegrity(certificate));
    s.append(LOG_ENTRY_TYPE_OBJ, "asdfhasdf", 10);
    s.getAppendedLength(&certificate);
    EXPECT_TRUE(s.checkMetadataIntegrity(certificate));

    // scribbling on an entry's data won't harm anything
    s.copyIn(2, "ASDFHASDF", 10);
    EXPECT_TRUE(s.checkMetadataIntegrity(certificate));

    // scribbling on metadata should result in a checksum error
    Segment::EntryHeader newHeader(LOG_ENTRY_TYPE_OBJTOMB, 10);
    s.copyIn(0, &newHeader, sizeof(newHeader));
    EXPECT_FALSE(s.checkMetadataIntegrity(certificate));
    EXPECT_TRUE(StringUtil::startsWith(TestLog::get(),
        "checkMetadataIntegrity: segment corrupt: bad checksum"));
}

TEST_P(SegmentTest, checkMetadataIntegrity_badLength) {
    TestLog::Enable _;
    SegmentAndAllocator segAndAlloc(GetParam());
    Segment& s = *segAndAlloc.segment;
    Segment::Certificate certificate;
    uint32_t segmentSize = GetParam()->segmentSize - 100;

    Segment::EntryHeader header(LOG_ENTRY_TYPE_OBJ, 1024*1024*1024);
    s.copyIn(0, &header, sizeof(header));
    s.copyIn(sizeof(header), &segmentSize, sizeof(segmentSize));
    s.head = 1;
    s.getAppendedLength(&certificate);
    EXPECT_FALSE(s.checkMetadataIntegrity(certificate));
    EXPECT_TRUE(StringUtil::startsWith(TestLog::get(),
        "checkMetadataIntegrity: segment corrupt: entries run off past "
        "expected length"));

    TestLog::reset();
    segmentSize = segAndAlloc.segmentSize;
    s.copyIn(0, &header, sizeof(header));
    s.copyIn(sizeof(header), &segmentSize, sizeof(segmentSize));
    s.getAppendedLength(&certificate);
    EXPECT_FALSE(s.checkMetadataIntegrity(certificate));
    EXPECT_TRUE(StringUtil::startsWith(TestLog::get(),
        "checkMetadataIntegrity: segment corrupt: entries run off past "
        "allocated segment size"));
}

} // namespace RAMCloud
