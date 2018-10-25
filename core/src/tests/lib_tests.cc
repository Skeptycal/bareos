/**
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2018-2018 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

#include "gtest/gtest.h"
#include "include/bareos.h"
#define BAREOS_TEST_LIB
#include "lib/bnet.h"
#include "lib/bstringlist.h"

TEST(BStringList, ConstructorsTest)
{
  BStringList list1;
  EXPECT_TRUE(list1.empty());

  list1.emplace_front(std::string("Test123"));
  EXPECT_EQ(0, list1.front().compare(std::string("Test123")));

  BStringList list2(list1);
  EXPECT_EQ(1, list2.size());
  EXPECT_EQ(0, list2.front().compare(std::string("Test123")));
}

TEST(BStringList, AppendTest)
{
  BStringList list1;
  std::vector<std::string> vec {"T", "est", "123"};
  list1.Append(vec);
  EXPECT_EQ(0, list1.front().compare(std::string("T")));
  list1.pop_front();
  EXPECT_EQ(0, list1.front().compare(std::string("est")));
  list1.pop_front();
  EXPECT_EQ(0, list1.front().compare(std::string("123")));

  BStringList list2;
  list2.Append('T');
  list2.Append('e');
  EXPECT_EQ(0, list2.front().compare(std::string("T")));
  list2.pop_front();
  EXPECT_EQ(0, list2.front().compare(std::string("e")));
}

TEST(BStringList, JoinTest)
{
  BStringList list1;
  list1 << "Test";
  list1 << 1 << 23;
  EXPECT_EQ(3, list1.size());
  EXPECT_STREQ(list1.Join().c_str(), "Test123");
  EXPECT_STREQ(list1.Join(' ').c_str(), "Test 1 23");

  BStringList list2;
  list2.Append("Test");
  list2.Append("123");

  std::string s = list2.Join(AsciiControlCharacters::RecordSeparator());
  EXPECT_EQ(8, s.size());

  std::string test {"Test"};
  test += AsciiControlCharacters::RecordSeparator(); // 0x1e
  test += "123";

  EXPECT_STREQ(s.c_str(), test.c_str());
}

TEST(BStringList, SplitStringTest)
{
  std::string test {"Test@123@String"};
  BStringList list1(test, '@');
  EXPECT_EQ(3, list1.size());

  EXPECT_STREQ("Test", list1.front().c_str());
  list1.pop_front();
  EXPECT_STREQ("123", list1.front().c_str());
  list1.pop_front();
  EXPECT_STREQ("String", list1.front().c_str());
}

TEST(BNet, ReadoutCommandIdFromStringTest)
{
  bool ok;
  uint32_t id;

  std::string message1 = "1000";
  message1 += 0x1e;
  message1 += "OK: <director-name> Version: <version>";
  BStringList list_of_arguments1(message1, 0x1e);
  ok = ReadoutCommandIdFromMessage(list_of_arguments1, id);
  EXPECT_EQ(id, kMessageIdOk);
  EXPECT_EQ(ok, true);

  std::string message2 = "1001";
  message2 += 0x1e;
  message2 += "OK: <director-name> Version: <version>";
  BStringList list_of_arguments2(message2, 0x1e);
  ok = ReadoutCommandIdFromMessage(list_of_arguments2, id);
  EXPECT_NE(id, kMessageIdOk);
  EXPECT_EQ(ok, true);
}

TEST(BNet, EvaluateResponseMessage_Wrong_Id)
{
  bool ok;
  uint32_t id;

  std::string message3 = "A1001";
  message3 += 0x1e;
  message3 += "OK: <director-name> Version: <version>";

  std::string human_readable_message;
  ok = EvaluateResponseMessageId(message3, id, human_readable_message);

  EXPECT_EQ(id, kMessageIdProtokollError);
  EXPECT_EQ(ok, false);

  const char *m3 {"A1001 OK: <director-name> Version: <version>"};
  EXPECT_STREQ(human_readable_message.c_str(), m3);
}

TEST(BNet, EvaluateResponseMessage_Correct_Id)
{
  bool ok;
  uint32_t id;

  std::string message4 = "1001";
  message4 += 0x1e;
  message4 += "OK: <director-name> Version: <version>";

  std::string human_readable_message;
  ok = EvaluateResponseMessageId(message4, id, human_readable_message);

  EXPECT_EQ(id, kMessageIdPamRequired);
  EXPECT_EQ(ok, true);

  const char *m3 {"1001 OK: <director-name> Version: <version>"};
  EXPECT_STREQ(human_readable_message.c_str(), m3);
}