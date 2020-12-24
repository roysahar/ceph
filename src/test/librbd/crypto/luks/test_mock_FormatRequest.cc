// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "test/librbd/test_mock_fixture.h"
#include "test/librbd/test_support.h"
#include "test/librbd/mock/MockImageCtx.h"

namespace librbd {
namespace util {

inline ImageCtx *get_image_ctx(MockImageCtx *image_ctx) {
  return image_ctx->image_ctx;
}

} // namespace util
} // namespace librbd

#include "librbd/crypto/luks/FormatRequest.cc"

namespace librbd {
namespace crypto {
namespace luks {

using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;

struct TestMockCryptoLuksFormatRequest : public TestMockFixture {
  typedef FormatRequest<librbd::MockImageCtx> MockFormatRequest;

  const size_t OBJECT_SIZE = 4 * 1024 * 1024;
  const size_t IMAGE_SIZE = 1024 * 1024 * 1024;
  const char* passphrase_cstr = "password";
  std::string passphrase = passphrase_cstr;

  MockImageCtx* mock_image_ctx;
  C_SaferCond finished_cond;
  Context *on_finish = &finished_cond;
  io::AioCompletion* aio_comp;
  ceph::bufferlist header_bl;

  void SetUp() override {
    TestMockFixture::SetUp();

    librbd::ImageCtx *ictx;
    ASSERT_EQ(0, open_image(m_image_name, &ictx));
    mock_image_ctx = new MockImageCtx(*ictx);
  }

  void TearDown() override {
    delete mock_image_ctx;
    TestMockFixture::TearDown();
  }

  void expect_get_object_size() {
    EXPECT_CALL(*mock_image_ctx, get_object_size()).WillOnce(Return(
            OBJECT_SIZE));
  }

  void expect_crypto_layer_exists_check(bool exists = false) {
    EXPECT_CALL(*mock_image_ctx->io_object_dispatcher, exists(
            io::OBJECT_DISPATCH_LAYER_CRYPTO)).WillOnce(Return(exists));
  }
                         
  void expect_get_image_size(uint64_t image_size) {
    EXPECT_CALL(*mock_image_ctx, get_image_size(CEPH_NOSNAP)).WillOnce(Return(
            image_size));
  }

  void expect_image_write() {
    EXPECT_CALL(*mock_image_ctx->io_image_dispatcher, send(_))
            .WillOnce(Invoke([this](io::ImageDispatchSpec* spec) {
                auto* write = boost::get<io::ImageDispatchSpec::Write>(
                        &spec->request);
                ASSERT_TRUE(write != nullptr);

                ASSERT_EQ(1, spec->image_extents.size());
                ASSERT_EQ(0, spec->image_extents[0].first);
                ASSERT_GT(spec->image_extents[0].second, 0);

                spec->dispatch_result = io::DISPATCH_RESULT_COMPLETE;
                aio_comp = spec->aio_comp;
                header_bl = write->bl;
            }));
  }

  void complete_aio(int r) {
    if (r < 0) {
      aio_comp->fail(r);
    } else {
      aio_comp->set_request_count(1);
      aio_comp->add_request();
      aio_comp->complete_request(r);
    }
  }

  void verify_header(size_t expected_key_length,
                     uint64_t expected_sector_size) {
    Header header(mock_image_ctx->cct);

    ASSERT_EQ(0, header.init());
    ASSERT_EQ(0, header.write(header_bl));
    ASSERT_EQ(0, header.load());

    ASSERT_EQ(expected_sector_size, header.get_sector_size());
    ASSERT_EQ(0, header.get_data_offset() % OBJECT_SIZE);

    char volume_key[64];
    size_t volume_key_size = sizeof(volume_key);
    ASSERT_EQ(0, header.read_volume_key(
            passphrase_cstr, strlen(passphrase_cstr),
            reinterpret_cast<char*>(volume_key), &volume_key_size));
  }
};

TEST_F(TestMockCryptoLuksFormatRequest, LUKS1) {
  auto mock_format_request = MockFormatRequest::create(
          mock_image_ctx, DiskEncryptionFormat::DISK_ENCRYPTION_FORMAT_LUKS1,
          CipherAlgorithm::CIPHER_ALGORITHM_AES128, std::move(passphrase),
          on_finish, true);
  expect_crypto_layer_exists_check();
  expect_get_object_size();
  expect_get_image_size(IMAGE_SIZE);
  expect_image_write();
  mock_format_request->send();
  ASSERT_EQ(ETIMEDOUT, finished_cond.wait_for(0));
  complete_aio(0);
  ASSERT_EQ(0, finished_cond.wait());
  ASSERT_NO_FATAL_FAILURE(verify_header(32, 512));
}

TEST_F(TestMockCryptoLuksFormatRequest, AES128) {
  auto mock_format_request = MockFormatRequest::create(
          mock_image_ctx, DiskEncryptionFormat::DISK_ENCRYPTION_FORMAT_LUKS2,
          CipherAlgorithm::CIPHER_ALGORITHM_AES128, std::move(passphrase),
          on_finish, true);
  expect_crypto_layer_exists_check();
  expect_get_object_size();
  expect_get_image_size(IMAGE_SIZE);
  expect_image_write();
  mock_format_request->send();
  ASSERT_EQ(ETIMEDOUT, finished_cond.wait_for(0));
  complete_aio(0);
  ASSERT_EQ(0, finished_cond.wait());
  ASSERT_NO_FATAL_FAILURE(verify_header(32, 4096));
}

TEST_F(TestMockCryptoLuksFormatRequest, AES256) {
  auto mock_format_request = MockFormatRequest::create(
          mock_image_ctx, DiskEncryptionFormat::DISK_ENCRYPTION_FORMAT_LUKS2,
          CipherAlgorithm::CIPHER_ALGORITHM_AES256, std::move(passphrase),
          on_finish, true);
  expect_crypto_layer_exists_check();
  expect_get_object_size();
  expect_get_image_size(IMAGE_SIZE);
  expect_image_write();
  mock_format_request->send();
  ASSERT_EQ(ETIMEDOUT, finished_cond.wait_for(0));
  complete_aio(0);
  ASSERT_EQ(0, finished_cond.wait());
  ASSERT_NO_FATAL_FAILURE(verify_header(62, 4096));
}

TEST_F(TestMockCryptoLuksFormatRequest, CryptoAlreadyLoaded) {
  auto mock_format_request = MockFormatRequest::create(
          mock_image_ctx, DiskEncryptionFormat::DISK_ENCRYPTION_FORMAT_LUKS2,
          CipherAlgorithm::CIPHER_ALGORITHM_AES256, std::move(passphrase),
          on_finish, true);
  expect_crypto_layer_exists_check(true);
  mock_format_request->send();
  ASSERT_EQ(-EEXIST, finished_cond.wait());
}

TEST_F(TestMockCryptoLuksFormatRequest, ImageTooSmall) {
  auto mock_format_request = MockFormatRequest::create(
          mock_image_ctx, RBD_ENCRYPTION_FORMAT_LUKS2,
          RBD_ENCRYPTION_ALGORITHM_AES256, std::move(passphrase), on_finish,
          true);
  expect_get_object_size();
  expect_get_image_size(1024*1024);
  mock_format_request->send();
  ASSERT_EQ(-ENOSPC, finished_cond.wait());
}

TEST_F(TestMockCryptoLuksFormatRequest, WriteFail) {
  auto mock_format_request = MockFormatRequest::create(
          mock_image_ctx, DiskEncryptionFormat::DISK_ENCRYPTION_FORMAT_LUKS2,
          CipherAlgorithm::CIPHER_ALGORITHM_AES256, std::move(passphrase),
          on_finish, true);
  expect_crypto_layer_exists_check();
  expect_get_object_size();
  expect_get_image_size(IMAGE_SIZE);
  expect_image_write();
  mock_format_request->send();
  ASSERT_EQ(ETIMEDOUT, finished_cond.wait_for(0));
  complete_aio(-123);
  ASSERT_EQ(-123, finished_cond.wait());
}

} // namespace luks
} // namespace crypto
} // namespace librbd
