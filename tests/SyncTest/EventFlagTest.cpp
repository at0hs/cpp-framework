#include "EventFlagTest.hpp"

TEST_F(EventFlagTest, GetSet) {
	const EFlag::Flag flag{ 0x01 };

	event_flag_.set(flag);

	EXPECT_EQ(flag, event_flag_.get());
}

TEST_F(EventFlagTest, And) {
	bool exited = false;

	std::thread t1{ [&] {
		event_flag_.wait(0x01, EFlag::MatchMode::AND);
		exited = true;
	} };
	event_flag_.set(0x01);
	t1.join();

	EXPECT_EQ(true, exited);
}

TEST_F(EventFlagTest, Or) {
	bool exited = false;

	std::thread t{ [&] {
		event_flag_.wait(0x11, EFlag::MatchMode::OR);
		exited = true;
	} };
	event_flag_.set(0x01);
	t.join();

	EXPECT_EQ(true, exited);
}

TEST_F(EventFlagTest, TimedOut_And) {
	bool ret = false;

	std::thread t{ [&] {
		ret = this->event_flag_.timed_wait(0x11, EFlag::MatchMode::AND, std::chrono::seconds(1));
	} };
	event_flag_.set(0x01);
	t.join();

	EXPECT_EQ(false, ret);
}

TEST_F(EventFlagTest, TimedOut_Or) {
	bool ret = false;

	std::thread t{ [&] {
		ret = this->event_flag_.timed_wait(0x02, EFlag::MatchMode::OR, std::chrono::seconds(1));
	} };
	event_flag_.set(0x01);
	t.join();

	EXPECT_EQ(false, ret);
}

TEST_F(EventFlagTest, WaitMultiple_1) {
	int exited = 0;

	std::function<void()> f{ [&] {
		if (this->event_flag_.timed_wait(0x01, EFlag::MatchMode::AND, std::chrono::seconds(1))) {
			exited++;
		}
	} };
	std::thread thread1{ f };
	std::thread thread2{ f };

	event_flag_.set(0x01);

	thread1.join();
	thread2.join();

	EXPECT_EQ(2, exited);
}

TEST_F(EventFlagTest, WaitMultiple_2) {
	int exited = 0;

	std::thread thread1{ [&] {
		if (this->event_flag_.timed_wait(0x01, EFlag::MatchMode::AND, std::chrono::seconds(1))) {
			exited++;
		}
	} };
	std::thread thread2{ [&] {
		if (this->event_flag_.timed_wait(0x02, EFlag::MatchMode::AND, std::chrono::seconds(1))) {
			exited++;
		}
	} };

	event_flag_.set(0x01);

	thread1.join();
	thread2.join();

	EXPECT_EQ(1, exited);
}

TEST_F(EventFlagTest, And_ExtraBitsSet_StillMatches) {
	// AND 待ちは指定ビットがすべて立っていれば成立する（他のビットが立っていても良い）
	event_flag_.set(0x01);
	event_flag_.set(0x04);

	EXPECT_TRUE(
		event_flag_.timed_wait(0x01, EFlag::MatchMode::AND, std::chrono::milliseconds(100)));
	EXPECT_TRUE(
		event_flag_.timed_wait(0x05, EFlag::MatchMode::AND, std::chrono::milliseconds(100)));
	EXPECT_FALSE(
		event_flag_.timed_wait(0x03, EFlag::MatchMode::AND, std::chrono::milliseconds(100)));
}
