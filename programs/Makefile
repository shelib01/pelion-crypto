
# To compile on SunOS: add "-lsocket -lnsl" to LDFLAGS
# To compile with PKCS11: add "-lpkcs11-helper" to LDFLAGS
ifdef TEST_FAULT
PROG_FAULT_OBJ ?= ../tests/data_files/platform_fault.o
CFLAGS	?= -O2 -I../tests/data_files
else
PROG_FAULT_OBJ ?=
CFLAGS	?= -O2
endif

WARNING_CFLAGS ?= -Wall -W -Wdeclaration-after-statement
WARNING_CXXFLAGS ?= -Wall -W
LDFLAGS ?=

# Check test environment. If ../library is available then Mbed TLS is used.
# Otherwise Mbed OS environment is used.
DIR_FOR_MBED_TLS_ENV=../library
ifneq "$(wildcard $(DIR_FOR_MBED_TLS_ENV) )" ""
	LIBRARY_DIR=../library
	INCLUDE_DIR=-I../include
else
	LIBRARY_DIR=../src
	INCLUDE_DIR=-I../inc
	CFLAGS += "-DMBEDTLS_CONFIG_FILE=\"mbedtls/test_config.h\""
endif

LOCAL_CFLAGS = $(WARNING_CFLAGS) $(INCLUDE_DIR) -D_FILE_OFFSET_BITS=64

LOCAL_CXXFLAGS = $(WARNING_CXXFLAGS) $(INCLUDE_DIR) -D_FILE_OFFSET_BITS=64
LOCAL_LDFLAGS = -L$(LIBRARY_DIR)	\
		-lmbedtls$(SHARED_SUFFIX)	\
		-lmbedx509$(SHARED_SUFFIX)	\
		-lmbedcrypto$(SHARED_SUFFIX)

ifndef SHARED
DEP=$(LIBRARY_DIR)/libmbedcrypto.a $(LIBRARY_DIR)/libmbedx509.a $(LIBRARY_DIR)/libmbedtls.a
else
DEP=$(LIBRARY_DIR)/libmbedcrypto.$(DLEXT) $(LIBRARY_DIR)/libmbedx509.$(DLEXT) $(LIBRARY_DIR)/libmbedtls.$(DLEXT)
endif

ifdef DEBUG
LOCAL_CFLAGS += -g3
endif

# if we're running on Windows, build for Windows
ifdef WINDOWS
WINDOWS_BUILD=1
endif

ifdef WINDOWS_BUILD
DLEXT=dll
EXEXT=.exe
LOCAL_LDFLAGS += -lws2_32
ifdef SHARED
SHARED_SUFFIX=.$(DLEXT)
endif
else
DLEXT ?= so
EXEXT=
SHARED_SUFFIX=
endif

# Zlib shared library extensions:
ifdef ZLIB
LOCAL_LDFLAGS += -lz
endif

APPS =	aes/aescrypt2$(EXEXT)		aes/crypt_and_hash$(EXEXT)	\
	hash/hello$(EXEXT)		hash/generic_sum$(EXEXT)	\
					pkey/dh_client$(EXEXT)		\
	pkey/dh_genprime$(EXEXT)	pkey/dh_server$(EXEXT)		\
	pkey/ecdh_curve25519$(EXEXT)					\
	pkey/ecdsa$(EXEXT)		pkey/gen_key$(EXEXT)		\
	pkey/key_app$(EXEXT)		pkey/key_app_writer$(EXEXT)	\
	pkey/mpi_demo$(EXEXT)		pkey/pk_decrypt$(EXEXT)		\
	pkey/pk_encrypt$(EXEXT)		pkey/pk_sign$(EXEXT)		\
	pkey/pk_verify$(EXEXT)		pkey/rsa_genkey$(EXEXT)		\
	pkey/rsa_decrypt$(EXEXT)	pkey/rsa_encrypt$(EXEXT)	\
	pkey/rsa_sign$(EXEXT)		pkey/rsa_verify$(EXEXT)		\
	pkey/rsa_sign_pss$(EXEXT)	pkey/rsa_verify_pss$(EXEXT)	\
	ssl/dtls_client$(EXEXT)		ssl/dtls_server$(EXEXT)		\
	ssl/ssl_client1$(EXEXT)		ssl/ssl_client2$(EXEXT)		\
	ssl/ssl_server$(EXEXT)		ssl/ssl_server2$(EXEXT)		\
	ssl/ssl_fork_server$(EXEXT)	ssl/mini_client$(EXEXT)		\
	ssl/ssl_mail_client$(EXEXT)	random/gen_entropy$(EXEXT)	\
	random/gen_random_havege$(EXEXT)				\
	random/gen_random_ctr_drbg$(EXEXT)				\
	test/benchmark$(EXEXT)                          		\
	test/selftest$(EXEXT)		test/udp_proxy$(EXEXT)		\
	test/zeroize$(EXEXT)						\
	test/query_compile_time_config$(EXEXT)				\
	util/pem2der$(EXEXT)		util/strerror$(EXEXT)		\
	x509/cert_app$(EXEXT)		x509/crl_app$(EXEXT)		\
	x509/cert_req$(EXEXT)		x509/cert_write$(EXEXT)		\
	x509/req_app$(EXEXT)

ifdef PTHREAD
APPS +=	ssl/ssl_pthread_server$(EXEXT)
endif

ifdef TEST_CPP
APPS += test/cpp_dummy_build$(EXEXT)
endif

.SILENT:

.PHONY: all clean list

all: $(APPS)

$(DEP):
	$(MAKE) -C ../library

aes/aescrypt2$(EXEXT): aes/aescrypt2.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    aes/aescrypt2.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) aes/aescrypt2.c  $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

aes/crypt_and_hash$(EXEXT): aes/crypt_and_hash.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    aes/crypt_and_hash.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) aes/crypt_and_hash.c $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

hash/hello$(EXEXT): hash/hello.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    hash/hello.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) hash/hello.c       $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

hash/generic_sum$(EXEXT): hash/generic_sum.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    hash/generic_sum.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) hash/generic_sum.c $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/dh_client$(EXEXT): pkey/dh_client.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/dh_client.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/dh_client.c   $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/dh_genprime$(EXEXT): pkey/dh_genprime.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/dh_genprime.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/dh_genprime.c $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/dh_server$(EXEXT): pkey/dh_server.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/dh_server.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/dh_server.c   $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/ecdh_curve25519$(EXEXT): pkey/ecdh_curve25519.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/ecdh_curve25519.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/ecdh_curve25519.c   $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/ecdsa$(EXEXT): pkey/ecdsa.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/ecdsa.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/ecdsa.c       $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/gen_key$(EXEXT): pkey/gen_key.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/gen_key.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/gen_key.c   $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/key_app$(EXEXT): pkey/key_app.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/key_app.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/key_app.c   $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/key_app_writer$(EXEXT): pkey/key_app_writer.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/key_app_writer.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/key_app_writer.c   $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/mpi_demo$(EXEXT): pkey/mpi_demo.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/mpi_demo.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/mpi_demo.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/pk_decrypt$(EXEXT): pkey/pk_decrypt.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/pk_decrypt.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/pk_decrypt.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/pk_encrypt$(EXEXT): pkey/pk_encrypt.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/pk_encrypt.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/pk_encrypt.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/pk_sign$(EXEXT): pkey/pk_sign.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/pk_sign.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/pk_sign.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/pk_verify$(EXEXT): pkey/pk_verify.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/pk_verify.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/pk_verify.c  $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/rsa_genkey$(EXEXT): pkey/rsa_genkey.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/rsa_genkey.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/rsa_genkey.c  $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/rsa_sign$(EXEXT): pkey/rsa_sign.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/rsa_sign.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/rsa_sign.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/rsa_verify$(EXEXT): pkey/rsa_verify.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/rsa_verify.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/rsa_verify.c  $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/rsa_sign_pss$(EXEXT): pkey/rsa_sign_pss.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/rsa_sign_pss.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/rsa_sign_pss.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/rsa_verify_pss$(EXEXT): pkey/rsa_verify_pss.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/rsa_verify_pss.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/rsa_verify_pss.c  $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/rsa_decrypt$(EXEXT): pkey/rsa_decrypt.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/rsa_decrypt.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/rsa_decrypt.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

pkey/rsa_encrypt$(EXEXT): pkey/rsa_encrypt.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    pkey/rsa_encrypt.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) pkey/rsa_encrypt.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

random/gen_entropy$(EXEXT): random/gen_entropy.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    random/gen_entropy.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) random/gen_entropy.c $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

random/gen_random_havege$(EXEXT): random/gen_random_havege.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    random/gen_random_havege.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) random/gen_random_havege.c $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

random/gen_random_ctr_drbg$(EXEXT): random/gen_random_ctr_drbg.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    random/gen_random_ctr_drbg.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) random/gen_random_ctr_drbg.c $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

ssl/dtls_client$(EXEXT): ssl/dtls_client.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    ssl/dtls_client.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) ssl/dtls_client.c  $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

ssl/dtls_server$(EXEXT): ssl/dtls_server.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    ssl/dtls_server.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) ssl/dtls_server.c  $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

ssl/ssl_client1$(EXEXT): ssl/ssl_client1.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    ssl/ssl_client1.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) ssl/ssl_client1.c  $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

ssl/ssl_client2$(EXEXT): ssl/ssl_client2.c ssl/query_config.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    ssl/ssl_client2.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) ssl/ssl_client2.c ssl/query_config.c $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

ssl/ssl_server$(EXEXT): ssl/ssl_server.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    ssl/ssl_server.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) ssl/ssl_server.c   $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

ssl/ssl_server2$(EXEXT): ssl/ssl_server2.c ssl/query_config.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    ssl/ssl_server2.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) ssl/ssl_server2.c ssl/query_config.c $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

ssl/ssl_fork_server$(EXEXT): ssl/ssl_fork_server.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    ssl/ssl_fork_server.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) ssl/ssl_fork_server.c   $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

ssl/ssl_pthread_server$(EXEXT): ssl/ssl_pthread_server.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    ssl/ssl_pthread_server.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) ssl/ssl_pthread_server.c   $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) -lpthread  $(LDFLAGS) -o $@

ssl/ssl_mail_client$(EXEXT): ssl/ssl_mail_client.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    ssl/ssl_mail_client.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) ssl/ssl_mail_client.c   $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

ssl/mini_client$(EXEXT): ssl/mini_client.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    ssl/mini_client.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) ssl/mini_client.c   $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

test/benchmark$(EXEXT): test/benchmark.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    test/benchmark.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) test/benchmark.c   $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

test/cpp_dummy_build$(EXEXT): test/cpp_dummy_build.cpp $(DEP) $(PROG_FAULT_OBJ)
	echo "  CXX   test/cpp_dummy_build.cpp"
	$(CXX) $(LOCAL_CXXFLAGS) $(CXXFLAGS) test/cpp_dummy_build.cpp   $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

test/selftest$(EXEXT): test/selftest.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    test/selftest.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) test/selftest.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

test/udp_proxy$(EXEXT): test/udp_proxy.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    test/udp_proxy.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) test/udp_proxy.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

test/zeroize$(EXEXT): test/zeroize.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    test/zeroize.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) test/zeroize.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

test/query_compile_time_config$(EXEXT): test/query_compile_time_config.c ssl/query_config.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    test/query_compile_time_config.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) test/query_compile_time_config.c ssl/query_config.c $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

util/pem2der$(EXEXT): util/pem2der.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    util/pem2der.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) util/pem2der.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

util/strerror$(EXEXT): util/strerror.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    util/strerror.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) util/strerror.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

x509/cert_app$(EXEXT): x509/cert_app.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    x509/cert_app.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) x509/cert_app.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

x509/cert_write$(EXEXT): x509/cert_write.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    x509/cert_write.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) x509/cert_write.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

x509/crl_app$(EXEXT): x509/crl_app.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    x509/crl_app.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) x509/crl_app.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

x509/cert_req$(EXEXT): x509/cert_req.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    x509/cert_req.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) x509/cert_req.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

x509/req_app$(EXEXT): x509/req_app.c $(DEP) $(PROG_FAULT_OBJ)
	echo "  CC    x509/req_app.c"
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) x509/req_app.c    $(PROG_FAULT_OBJ) $(LOCAL_LDFLAGS) $(LDFLAGS) -o $@

../tests/data_files/platform_fault.o:
	$(MAKE) -C ../tests data_files/platform_fault.o

clean:
ifndef WINDOWS
	rm -f $(APPS)
	-rm -f ssl/ssl_pthread_server$(EXEXT)
	-rm -f test/cpp_dummy_build$(EXEXT)
	-rm -f *.su
else
	del /S /Q /F *.o *.su *.exe
endif

list:
	echo $(APPS)
