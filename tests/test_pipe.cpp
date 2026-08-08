/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonpipe_test_utils.h"
#include "player/dragonpipe.h"

#include <QtTest>

#include <algorithm>
#include <array>
#include <chrono>
#include <ranges>
#include <stop_token>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

class TestDragonPipe : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    void testDefaultCapacity()
    {
        DragonPipe<float> pipe;
        QCOMPARE_EQ(pipe.producer().available(), size_t{65536});
        QCOMPARE_EQ(pipe.consumer().ready(), size_t{0});
    }

    void testCustomCapacity()
    {
        DragonPipe<float> pipe(256);
        QCOMPARE_EQ(pipe.producer().available(), size_t{256});
    }

    void testWriteReadSome()
    {
        DragonPipe<float> pipe(1024);

        std::array<float, 8> input;
        std::ranges::generate(input, [n = 1.0f]() mutable {
            return n++;
        });

        size_t n = writeAll(pipe.producer(), input);
        QCOMPARE_EQ(n, size_t{8});
        QCOMPARE_EQ(pipe.consumer().ready(), size_t{8});

        size_t read = pipe.consumer().readSomeWith(8, [&](std::span<const float> b1, std::span<const float> b2) {
            size_t total = b1.size() + b2.size();
            QCOMPARE_EQ(total, size_t{8});
            for (size_t i = 0; i < total; ++i) {
                float expected = static_cast<float>(i + 1);
                float got = i < b1.size() ? b1[i] : b2[i - b1.size()];
                QCOMPARE_EQ(got, expected);
            }
        });
        QCOMPARE_EQ(read, size_t{8});
        QCOMPARE_EQ(pipe.consumer().ready(), size_t{0});
    }

    void testNonBlockingWriteSmallData()
    {
        DragonPipe<float> pipe(16);

        std::array<float, 8> data;
        std::ranges::generate(data, [n = 1.0f]() mutable {
            return n++;
        });

        std::stop_source ss;
        size_t n = pipe.producer().write(data, ss.get_token());
        QCOMPARE_EQ(n, size_t{8});
        QCOMPARE_EQ(pipe.consumer().ready(), size_t{8});
    }

    void testBlockingWriteBackpressure()
    {
        DragonPipe<float> pipe(16);

        std::array<float, 64> data;
        std::ranges::generate(data, [n = 1.0f]() mutable {
            return n++;
        });

        std::stop_source ss;

        std::vector<float> consumedData;
        consumedData.reserve(64);

        std::jthread consumer([&](std::stop_token st) {
            while (consumedData.size() < 64 && !st.stop_requested()) {
                if (pipe.consumer().ready() >= 4) {
                    pipe.consumer().readSomeWith(4, [&](std::span<float> b1, std::span<float> b2) {
                        for (size_t i = 0; i < b1.size(); ++i)
                            consumedData.push_back(b1[i]);
                        for (size_t i = 0; i < b2.size(); ++i)
                            consumedData.push_back(b2[i]);
                    });
                } else {
                    std::this_thread::sleep_for(100us);
                }
            }
        });

        size_t n = pipe.producer().write(data, ss.get_token());
        QCOMPARE_EQ(n, size_t{64});

        consumer.join();

        QCOMPARE_EQ(consumedData.size(), size_t{64});
        for (size_t i = 0; i < 64; ++i) {
            QCOMPARE_EQ(consumedData[i], static_cast<float>(i + 1));
        }
    }

    void testBlockingWriteStopRequested()
    {
        DragonPipe<float> pipe(8);

        std::vector<float> big(1000, 1.0f);

        std::stop_source ss;

        std::jthread canceller([&] {
            std::this_thread::sleep_for(5ms);
            ss.request_stop();
        });

        size_t n = pipe.producer().write(big, ss.get_token());
        QVERIFY(n < 1000);

        canceller.join();
    }

    void testWaitForWakesOnData()
    {
        DragonPipe<float> pipe(256);

        std::stop_source ss;

        std::jthread producer([&] {
            std::this_thread::sleep_for(5ms);
            std::array<float, 4> d{1, 2, 3, 4};
            writeAll(pipe.producer(), d);
        });

        bool ok = pipe.consumer().waitFor(4, ss.get_token());
        QVERIFY(ok);
        QVERIFY(pipe.consumer().ready() >= 4);

        producer.join();
    }

    void testWaitForTimeout()
    {
        DragonPipe<float> pipe(64);

        std::stop_source ss;

        bool ok = pipe.consumer().waitFor(4, ss.get_token());
        QVERIFY(!ok);
    }

    void testWaitForStopRequested()
    {
        DragonPipe<float> pipe(64);

        std::stop_source ss;

        std::jthread stopper([&] {
            std::this_thread::sleep_for(5ms);
            ss.request_stop();
            pipe.producer().notify();
        });

        bool ok = pipe.consumer().waitFor(4, ss.get_token());
        QVERIFY(!ok);

        stopper.join();
    }

    void testDrain()
    {
        DragonPipe<float> pipe(128);

        std::array<float, 16> data;
        std::ranges::generate(data, [n = 1.0f]() mutable {
            return n++;
        });
        writeAll(pipe.producer(), data);

        QCOMPARE_EQ(pipe.consumer().ready(), size_t{16});
        pipe.consumer().drain();
        QCOMPARE_EQ(pipe.consumer().ready(), size_t{0});
    }

    void testProducerNotify()
    {
        DragonPipe<float> pipe(128);

        std::stop_source ss;
        std::atomic<bool> enteredWait{false};
        std::atomic<bool> waitReturned{false};

        std::jthread waiter([&] {
            enteredWait.store(true);
            pipe.consumer().waitFor(999999, ss.get_token());
            waitReturned.store(true);
        });

        std::this_thread::sleep_for(5ms);
        QVERIFY2(enteredWait.load(), "Consumer thread should have entered waitFor");

        ss.request_stop();
        pipe.producer().notify();

        QTRY_VERIFY_WITH_TIMEOUT(waitReturned.load(), 2000);
        waiter.join();
    }

    void testAvailable()
    {
        DragonPipe<float> pipe(16);
        QCOMPARE_EQ(pipe.producer().available(), size_t{16});

        std::array<float, 4> d;
        writeAll(pipe.producer(), d);
        QCOMPARE_EQ(pipe.producer().available(), size_t{12});
    }

    void testWrapAround()
    {
        DragonPipe<float> pipe(64);

        std::array<float, 70> writeBuf;
        std::ranges::generate(writeBuf, [n = 1.0f]() mutable {
            return n++;
        });

        std::array<float, 70> readBuf{};

        std::stop_source ss;
        std::jthread consumer([&](std::stop_token st) {
            size_t totalRead = 0;
            while (totalRead < 70 && !st.stop_requested()) {
                if (pipe.consumer().ready() >= 10) {
                    size_t n = pipe.consumer().readSomeWith(10, [&](std::span<const float> b1, std::span<const float> b2) {
                        size_t offset = totalRead;
                        auto it = std::copy(b1.begin(), b1.end(), readBuf.begin() + static_cast<long>(offset));
                        std::copy(b2.begin(), b2.end(), it);
                    });
                    totalRead += n;
                } else {
                    std::this_thread::sleep_for(100us);
                }
            }
        });

        size_t written = pipe.producer().write(writeBuf, ss.get_token());
        QCOMPARE_EQ(written, size_t{70});

        consumer.join();

        for (size_t i = 0; i < 70; ++i) {
            QCOMPARE_EQ(readBuf[i], static_cast<float>(i + 1));
        }
    }

    void testWriteSomeWith()
    {
        DragonPipe<float> pipe(64);

        size_t n = pipe.producer().writeSomeWith(8, [](std::span<float> b1, std::span<float> b2) {
            for (size_t i = 0; i < b1.size(); ++i)
                b1[i] = static_cast<float>(i + 1);
            for (size_t i = 0; i < b2.size(); ++i)
                b2[i] = static_cast<float>(b1.size() + i + 1);
        });
        QCOMPARE_EQ(n, size_t{8});
        QCOMPARE_EQ(pipe.consumer().ready(), size_t{8});

        std::vector<float> readData;
        pipe.consumer().readSomeWith(8, [&](std::span<float> b1, std::span<float> b2) {
            for (auto v : b1)
                readData.push_back(v);
            for (auto v : b2)
                readData.push_back(v);
        });
        QCOMPARE_EQ(readData.size(), size_t{8});
        for (size_t i = 0; i < readData.size(); ++i) {
            QCOMPARE_EQ(readData[i], static_cast<float>(i + 1));
        }
    }

    void testMultiThreadedCycle()
    {
        DragonPipe<float> pipe(256);

        constexpr int kRounds = 5;
        constexpr int kBatchSize = 16;

        std::atomic<int> producerDone{0};
        std::atomic<int> consumerDone{0};
        std::atomic<bool> mismatchDetected{false};
        std::atomic<int> totalRead{0};

        std::jthread producer([&](std::stop_token st) {
            for (int round = 0; round < kRounds; ++round) {
                std::array<float, kBatchSize> data;
                std::ranges::generate(data, [n = static_cast<float>(round * kBatchSize + 1)]() mutable {
                    return n++;
                });
                pipe.producer().write(data, st);
            }
            producerDone.store(1);
        });

        std::jthread consumerThread([&](std::stop_token st) {
            int round = 0;
            while (round < kRounds) {
                if (!pipe.consumer().waitFor(kBatchSize, st)) {
                    if (st.stop_requested())
                        break;
                    continue;
                }
                size_t read = pipe.consumer().readSomeWith(kBatchSize, [&](std::span<const float> b1, std::span<const float> b2) {
                    if (b1.size() + b2.size() != size_t{kBatchSize})
                        mismatchDetected.store(true);
                    float expected = static_cast<float>(round * kBatchSize + 1);
                    float first = b1.empty() ? b2.front() : b1.front();
                    if (first != expected)
                        mismatchDetected.store(true);
                });
                if (read != size_t{kBatchSize})
                    mismatchDetected.store(true);
                totalRead.fetch_add(static_cast<int>(read), std::memory_order_relaxed);
                ++round;
            }
            consumerDone.store(1);
        });

        auto deadline = std::chrono::steady_clock::now() + 2s;
        while ((producerDone.load() == 0 || consumerDone.load() == 0) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }

        if (producerDone.load() == 0)
            producer.request_stop();
        if (consumerDone.load() == 0)
            consumerThread.request_stop();

        producer.join();
        consumerThread.join();

        QCOMPARE_EQ(producerDone.load(), 1);
        QCOMPARE_EQ(consumerDone.load(), 1);
        QVERIFY2(!mismatchDetected.load(), "Data mismatch or size error detected in multi-threaded cycle");
        QCOMPARE_EQ(totalRead.load(), kRounds * kBatchSize);
    }
};

QTEST_MAIN(TestDragonPipe)
#include "test_pipe.moc"
