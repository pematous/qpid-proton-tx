# Transaction Tests - Quick Start Guide

## What's Been Added

This fork contains comprehensive tests for the C++ client local transaction support feature:

### New Files
1. **`cpp/src/transaction_test.cpp`** - Original basic transaction test
2. **`cpp/src/transaction_test_enhanced.cpp`** - Enhanced comprehensive test suite
3. **`cpp/TRANSACTION_TEST_COVERAGE.md`** - Detailed test coverage documentation
4. **`cpp/tests.cmake`** - Updated to include transaction tests

### Test Coverage

The enhanced test suite includes:
- ✅ Basic transaction lifecycle (declare, commit, abort)
- ✅ Transaction ID retrieval and validation
- ✅ Multiple sequential transactions
- ✅ Error handling (double declare, commit/abort without declare)
- ✅ Message tracking within transactions
- ✅ Proper resource cleanup

## Building the Tests

### Prerequisites
- CMake 3.x or higher
- C++11 compatible compiler
- Qpid Proton C library

### Build Steps

```bash
# Navigate to your fork
cd /Users/pematous/Work/BOB/qpid-proton-tx

# Create build directory
mkdir -p build
cd build

# Configure with CMake
cmake ..

# Build the transaction tests
make transaction_test transaction_test_enhanced

# Or build all tests
make
```

## Running the Tests

### Run All Transaction Tests
```bash
cd build
ctest -R transaction -V
```

### Run Individual Test Executables
```bash
# Run original test
./cpp/transaction_test

# Run enhanced test suite
./cpp/transaction_test_enhanced
```

### Run Specific Test Cases
The enhanced test supports running individual test cases:
```bash
./cpp/transaction_test_enhanced test_basic_commit
./cpp/transaction_test_enhanced test_basic_abort
./cpp/transaction_test_enhanced test_double_declare
```

## Expected Output

### Successful Test Run
```
========================================
Enhanced Transaction Test Suite
========================================

=== TEST: Basic Transaction Commit ===
[BROKER] Transaction declared (#0): prqs5678-abcd-efgh-1a2b-3c4d5e6f7g8e
[CLIENT] Transaction declared: prqs5678-abcd-efgh-1a2b-3c4d5e6f7g8e
[CLIENT] Sent message 1/3
[CLIENT] Sent message 2/3
[CLIENT] Sent message 3/3
[CLIENT] Committing transaction
[BROKER] Transaction committed: prqs5678-abcd-efgh-1a2b-3c4d5e6f7g8e
[CLIENT] Transaction committed successfully
✓ Basic commit test passed

... (more tests)

========================================
✓ All tests passed!
========================================
```

## Test Architecture

### Components

1. **EnhancedFakeBroker**
   - Simulates AMQP broker behavior
   - Supports transaction coordination
   - Tracks messages per transaction
   - Configurable for error scenarios

2. **Test Clients**
   - BasicTransactionClient: Simple transaction operations
   - MultiTransactionClient: Sequential transaction testing
   - ErrorTestClient: Error condition validation

3. **Synchronization**
   - Promise-based coordination between broker and clients
   - Timeout protection (5 seconds default)
   - Thread-safe operation

## Troubleshooting

### Build Errors

**Problem**: Cannot find proton headers
```
Solution: Ensure Qpid Proton C library is installed and CMake can find it
```

**Problem**: Linker errors with qpid-proton-core
```
Solution: Check that qpid-proton-core library is built and available
```

### Test Failures

**Problem**: Timeout errors
```
Possible causes:
- Deadlock in test logic
- Broker not starting properly
- Port already in use
```

**Problem**: Assertion failures
```
Check the specific assertion and verify:
- Transaction state is as expected
- Message counts are correct
- Transaction IDs match
```

### Running with Valgrind
To check for memory leaks:
```bash
valgrind --leak-check=full ./cpp/transaction_test_enhanced
```

## Next Steps

### Additional Tests to Implement

1. **Transactional Delivery Tests**
   - Test `on_transactional_accept()` handler
   - Test `on_transactional_reject()` handler
   - Test `on_transactional_release()` handler

2. **settle_before_discharge Tests**
   - Test with parameter set to true
   - Test with parameter set to false
   - Validate settlement timing

3. **Receiver Transaction Tests**
   - Transactional message receiving
   - Accept/reject/release in transactions

4. **Integration Tests**
   - Test with real AMQP broker (ActiveMQ Artemis, Qpid Broker-J)
   - Interoperability testing

### Contributing

To add new tests:
1. Follow the existing test structure in `transaction_test_enhanced.cpp`
2. Add test documentation to `TRANSACTION_TEST_COVERAGE.md`
3. Use descriptive test names
4. Ensure proper cleanup and synchronization
5. Test both success and failure paths

## Files Modified/Added

```
cpp/
├── src/
│   ├── transaction_test.cpp              (new - copied from original)
│   └── transaction_test_enhanced.cpp     (new - comprehensive tests)
├── tests.cmake                            (modified - added transaction tests)
├── TRANSACTION_TEST_COVERAGE.md          (new - test documentation)
└── TRANSACTION_TESTS_README.md           (new - this file)
```

## References

- [AMQP 1.0 Transaction Specification](http://docs.oasis-open.org/amqp/core/v1.0/os/amqp-core-transactions-v1.0-os.html)
- [Qpid Proton C++ API](https://qpid.apache.org/releases/qpid-proton-0.39.0/proton/cpp/api/index.html)
- Original implementation in `cpp/src/session.cpp`

## Contact

For questions or issues with these tests, please refer to the test coverage documentation
or examine the test implementation for detailed behavior.