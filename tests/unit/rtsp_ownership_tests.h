// Included by rtsp.cpp to exercise the socket boundary without a host runtime.
#include <gtest/gtest.h>

namespace rtsp_stream {
  TEST(RtspOwnership, UnrelatedSocketFailuresPreserveEncryptedHandshake) {
    boost::asio::io_context io;
    auto launch = std::make_shared<launch_session_t>();
    const crypto::aes_t key(16, 0x42);
    launch->rtsp_cipher.emplace(key, false);
    launch->rtsp_iv_counter = 0;
    bool handled = false;
    auto legitimate = std::make_shared<socket_t>(io, [&](tcp::socket &, launch_session_t &session, msg_t &&request) {
      EXPECT_EQ(&session, launch.get());
      EXPECT_FALSE(session.aborted.load());
      EXPECT_STREQ(request->message.request.command, "OPTIONS");
      handled = true;
    });
    legitimate->session = launch;
    legitimate->msg_buf.fill(0);

    const std::string_view request = "OPTIONS rtsp://localhost RTSP/1.0\r\nCSeq: 1\r\n\r\n";
    auto *header = reinterpret_cast<encrypted_rtsp_header_t *>(legitimate->msg_buf.data());
    *header = {};
    header->typeAndLength = util::endian::big<uint32_t>(encrypted_rtsp_header_t::ENCRYPTED_MESSAGE_TYPE_BIT | static_cast<uint32_t>(request.size()));
    header->sequenceNumber = util::endian::big<uint32_t>(1);
    crypto::aes_t iv(12);
    const uint32_t sequence = 1;
    std::copy_n(reinterpret_cast<const uint8_t *>(&sequence), sizeof(sequence), iv.begin());
    iv[10] = 'C';
    iv[11] = 'R';
    crypto::cipher::gcm_t client_cipher(key, false);
    ASSERT_EQ(client_cipher.encrypt(request, header->tag, header->payload(), &iv), static_cast<int>(request.size()));

    // Each accepted connection borrows the same pending launch, even before it
    // has presented an authenticated request.
    for (int failure = 0; failure < 3; ++failure) {
      auto unrelated = std::make_shared<socket_t>(io, [](tcp::socket &, launch_session_t &, msg_t &&) {
        ADD_FAILURE() << "Unauthenticated request reached dispatch";
      });
      unrelated->session = launch;
      if (failure == 1) {
        unrelated->msg_buf.fill(0); // Invalid encrypted header; response write also fails.
        socket_t::handle_read_encrypted_header(unrelated, {}, sizeof(encrypted_rtsp_header_t));
      } else if (failure == 2) {
        unrelated->msg_buf = legitimate->msg_buf;
        auto *invalid = reinterpret_cast<encrypted_rtsp_header_t *>(unrelated->msg_buf.data());
        invalid->tag[0] ^= 1;
        socket_t::handle_read_encrypted_message(unrelated, {}, request.size());
      }
      unrelated.reset(); // Includes accept followed by close without any data.
      EXPECT_FALSE(launch->aborted.load());
    }

    socket_t::handle_read_encrypted_message(legitimate, {}, request.size());
    EXPECT_TRUE(handled);
    EXPECT_FALSE(launch->aborted.load());
  }
}  // namespace rtsp_stream

namespace rtsp_stream {
  TEST(RtspOwnership, DuplicateAdmissionPreservesStartedLaunch) {
    rtsp_server_t server;
    launch_session_t launch {};
    launch.desktop = {7, 1};
    launch.started = true;
    // The admission boundary also checks duplicates that passed ANNOUNCE's
    // initial pending check before another request claimed the launch.
    EXPECT_FALSE(server.start_session({}, launch, "127.0.0.1"));
    EXPECT_TRUE(launch.started.load());
    EXPECT_FALSE(launch.aborted.load());
    EXPECT_EQ(launch.desktop.epoch, 7u);
    EXPECT_EQ(launch.desktop.attempt, 1u);
    EXPECT_EQ(server.session_count(), 0);
  }

  TEST(RtspOwnership, ShutdownAdmissionCleansOnlyTheClaimedAttempt) {
    rtsp_server_t server;
    auto shutdown = mail::man->event<bool>(mail::shutdown);
    shutdown->raise(true);
    auto restore = util::fail_guard([&] { shutdown->try_pop(); });
    launch_session_t original {}, pending {};
    original.started = true;
    EXPECT_FALSE(server.start_session({}, original, "127.0.0.1"));
    EXPECT_FALSE(original.aborted.load());
    EXPECT_FALSE(server.start_session({}, pending, "127.0.0.1"));
    EXPECT_TRUE(pending.aborted.load());
    EXPECT_TRUE(pending.started.load());
    EXPECT_EQ(server.session_count(), 0);
  }
}  // namespace rtsp_stream
