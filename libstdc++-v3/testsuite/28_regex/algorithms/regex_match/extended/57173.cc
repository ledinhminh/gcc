// { dg-options "-std=gnu++11" }

//
// 2013-07-23  Tim Shen <timshen91@gmail.com>
//
// Copyright (C) 2013 Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Library.  This library is free
// software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the
// Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this library; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.

// 28.11.3 regex_search
// Tests Extended against a std::string target.

#include <regex>
#include <testsuite_hooks.h>
#include <iostream>

// libstdc++/57173
void
test01()
{
  bool test __attribute__((unused)) = true;

  {
    std::regex  re("/asdf(/.*)", std::regex::extended);
    std::string target("/asdf/qwerty");
    std::smatch m;

    VERIFY( std::regex_match(target, m, re) );
    VERIFY( m.size() == 2 );
    VERIFY( std::string(m[1].first, m[1].second) == "/qwerty");
  }
  {
    std::regex  re("/asdf(/.*)()\\2", std::regex::extended);
    std::string target("/asdf/qwerty");
    std::smatch m;

    VERIFY( std::regex_match(target, m, re) );
    VERIFY( m.size() == 3 );
    VERIFY( std::string(m[1].first, m[1].second) == "/qwerty");
  }
}

int
main()
{
  test01();
  return 0;
}
