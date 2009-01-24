require File.dirname(__FILE__) + '/../spec_helper'

describe "A Op_asgn1 node" do
  relates <<-ruby do
      @b = []
      @b[1] ||= 10
      @b[2] &&= 11
      @b[3] += 12
    ruby

    parse do
      [:block,
       [:iasgn, :@b, [:array]],
       [:op_asgn1, [:ivar, :@b], [:arglist, [:lit, 1]], :"||", [:lit, 10]],
       [:op_asgn1, [:ivar, :@b], [:arglist, [:lit, 2]], :"&&", [:lit, 11]],
       [:op_asgn1, [:ivar, :@b], [:arglist, [:lit, 3]], :+, [:lit, 12]]]
    end

    compile do |g|
      l_or = g.new_label
      l_and = g.new_label
      l_idx = g.new_label
      l_rhs = g.new_label

      g.make_array 0
      g.set_ivar :@b
      g.pop

      g.push_ivar :@b
      g.dup
      g.push 1
      g.send :[], 1

      g.dup
      g.git l_or
      g.pop

      g.push 1
      g.push 10
      g.send :[]=, 2

      g.goto l_and

      l_or.set!

      g.swap
      g.pop

      l_and.set!

      g.pop
      g.push_ivar :@b
      g.dup
      g.push 2
      g.send :[], 1
      g.dup
      g.gif l_idx

      g.pop
      g.push 2
      g.push 11
      g.send :[]=, 2
      g.goto l_rhs

      l_idx.set!

      g.swap
      g.pop

      l_rhs.set!

      g.pop
      g.push_ivar :@b
      g.dup
      g.push 3
      g.send :[], 1

      g.push 12
      g.send :+, 1

      g.push 3
      g.swap
      g.send :[]=, 2
    end
  end

  relates <<-ruby do
      b = []
      b[1] ||= 10
      b[2] &&= 11
      b[3] += 12
    ruby

    parse do
      [:block,
       [:lasgn, :b, [:array]],
       [:op_asgn1, [:lvar, :b], [:arglist, [:lit, 1]], :"||", [:lit, 10]],
       [:op_asgn1, [:lvar, :b], [:arglist, [:lit, 2]], :"&&", [:lit, 11]],
       [:op_asgn1, [:lvar, :b], [:arglist, [:lit, 3]], :+, [:lit, 12]]]
    end

    compile do |g|
      l_or = g.new_label
      l_and = g.new_label
      l_idx = g.new_label
      l_rhs = g.new_label

      g.make_array 0
      g.set_local 0
      g.pop

      g.push_local 0
      g.dup
      g.push 1
      g.send :[], 1

      g.dup
      g.git l_or
      g.pop

      g.push 1
      g.push 10
      g.send :[]=, 2

      g.goto l_and

      l_or.set!

      g.swap
      g.pop

      l_and.set!

      g.pop
      g.push_local 0
      g.dup
      g.push 2
      g.send :[], 1
      g.dup
      g.gif l_idx

      g.pop
      g.push 2
      g.push 11
      g.send :[]=, 2
      g.goto l_rhs

      l_idx.set!

      g.swap
      g.pop

      l_rhs.set!

      g.pop
      g.push_local 0
      g.dup
      g.push 3
      g.send :[], 1

      g.push 12
      g.send :+, 1

      g.push 3
      g.swap
      g.send :[]=, 2
    end
  end
end

describe "A Op_asgn2 node" do
  relates "self.Bag ||= Bag.new" do
    parse do
      [:op_asgn2,
        [:self],
        :"Bag=", :"||", [:call, [:const, :Bag], :new, [:arglist]]]
    end

    compile do |g|
      t = g.new_label
      f = g.new_label

      g.push :self
      g.dup
      g.send :Bag, 0
      g.dup
      g.git t
      g.pop
      g.push_const :Bag
      g.send :new, 0, false
      g.send :"Bag=", 1
      g.goto f

      t.set!

      g.swap
      g.pop

      f.set!
    end
  end

  relates <<-ruby do
      s = Struct.new(:var)
      c = s.new(nil)
      c.var ||= 20
      c.var &&= 21
      c.var += 22
      c.d.e.f ||= 42
    ruby

    parse do
      [:block,
       [:lasgn, :s, [:call, [:const, :Struct], :new, [:arglist, [:lit, :var]]]],
       [:lasgn, :c, [:call, [:lvar, :s], :new, [:arglist, [:nil]]]],
       [:op_asgn2, [:lvar, :c], :var=, :"||", [:lit, 20]],
       [:op_asgn2, [:lvar, :c], :var=, :"&&", [:lit, 21]],
       [:op_asgn2, [:lvar, :c], :var=, :+, [:lit, 22]],
       [:op_asgn2,
        [:call, [:call, [:lvar, :c], :d, [:arglist]], :e, [:arglist]],
        :f=,
        :"||",
        [:lit, 42]]]
    end

    compile do |g|
      l_or = g.new_label
      l_and = g.new_label
      l_plus = g.new_label
      l_or2 = g.new_label
      l_rhs = g.new_label
      bottom = g.new_label

      g.push_const :Struct
      g.push_unique_literal :var
      g.send :new, 1, false
      g.set_local 0
      g.pop

      g.push_local 0
      g.push :nil
      g.send :new, 1, false
      g.set_local 1
      g.pop

      g.push_local 1
      g.dup
      g.send :var, 0
      g.dup
      g.git l_or

      g.pop
      g.push 20
      g.send :var=, 1
      g.goto l_and

      l_or.set!

      g.swap
      g.pop

      l_and.set!

      g.pop
      g.push_local 1
      g.dup
      g.send :var, 0
      g.dup
      g.gif l_plus
      g.pop
      g.push 21
      g.send :var=, 1
      g.goto l_or2

      l_plus.set!

      g.swap
      g.pop

      l_or2.set!

      g.pop
      g.push_local 1
      g.dup
      g.send :var, 0
      g.push 22
      g.send :+, 1
      g.send :var=, 1
      g.pop

      g.push_local 1
      g.send :d, 0, false
      g.send :e, 0, false
      g.dup
      g.send :f, 0
      g.dup

      g.git l_rhs

      g.pop
      g.push 42
      g.send :f=, 1
      g.goto bottom

      l_rhs.set!

      g.swap
      g.pop

      bottom.set!
    end
  end
end

describe "A Op_asgn_and node" do
  relates "@fetcher &&= new(Gem.configuration[:http_proxy])" do
    parse do
      [:op_asgn_and,
       [:ivar, :@fetcher],
       [:iasgn,
        :@fetcher,
        [:call,
         nil,
         :new,
         [:arglist,
          [:call,
           [:call, [:const, :Gem], :configuration, [:arglist]],
           :[],
           [:arglist, [:lit, :http_proxy]]]]]]]
    end

    compile do |g|
      t = g.new_label

      g.push_ivar :@fetcher
      g.dup
      g.gif t
      g.pop

      g.push :self
      g.push_const :Gem
      g.send :configuration, 0, false

      g.push_unique_literal :http_proxy
      g.send :[], 1, false

      g.send :new, 1, true

      g.set_ivar :@fetcher

      t.set!
    end
  end

  relates <<-ruby do
      a = 0
      a &&= 2
    ruby

    parse do
      [:block,
       [:lasgn, :a, [:lit, 0]],
       [:op_asgn_and, [:lvar, :a], [:lasgn, :a, [:lit, 2]]]]
    end

    compile do |g|
      g.push 0
      g.set_local 0
      g.pop

      g.push_local 0
      g.dup

      f = g.new_label
      g.gif f
      g.pop
      g.push 2
      g.set_local 0

      f.set!
    end
  end
end

describe "A Op_asgn_or node" do
  relates <<-ruby do
      a ||= begin
              b
            rescue
              c
            end
    ruby

    parse do
      [:op_asgn_or,
       [:lvar, :a],
       [:lasgn,
        :a,
        [:rescue,
         [:call, nil, :b, [:arglist]],
         [:resbody, [:array], [:call, nil, :c, [:arglist]]]]]]
    end

    compile do |g|
      t = g.new_label

      g.push_local 0
      g.dup
      g.git t
      g.pop

      in_rescue :StandardError, 1 do |section|
        case section
        when :body then
          g.push :self
          g.send :b, 0, true
        when :StandardError then
          g.push :self
          g.send :c, 0, true
        end
      end

      g.set_local 0

      t.set!
    end
  end

  relates "@fetcher ||= new(Gem.configuration[:http_proxy])" do
    parse do
      [:op_asgn_or,
       [:ivar, :@fetcher],
       [:iasgn,
        :@fetcher,
        [:call,
         nil,
         :new,
         [:arglist,
          [:call,
           [:call, [:const, :Gem], :configuration, [:arglist]],
           :[],
           [:arglist, [:lit, :http_proxy]]]]]]]
    end

    compile do |g|
      t = g.new_label

      g.push_ivar :@fetcher
      g.dup
      g.git t
      g.pop

      g.push :self
      g.push_const :Gem
      g.send :configuration, 0, false

      g.push_unique_literal :http_proxy
      g.send :[], 1, false

      g.send :new, 1, true

      g.set_ivar :@fetcher

      t.set!
    end
  end

  relates "@v ||= {  }" do
    parse do
      [:op_asgn_or, [:ivar, :@v], [:iasgn, :@v, [:hash]]]
    end

    compile do |g|
      t = g.new_label

      g.push_ivar :@v
      g.dup
      g.git t
      g.pop

      g.push_cpath_top
      g.find_const :Hash
      g.send :[], 0

      g.set_ivar :@v

      t.set!
    end
  end

  relates <<-ruby do
      a = 0
      a ||= 1
    ruby

    parse do
      [:block,
       [:lasgn, :a, [:lit, 0]],
       [:op_asgn_or, [:lvar, :a], [:lasgn, :a, [:lit, 1]]]]
    end

    compile do |g|
      t = g.new_label

      g.push 0
      g.set_local 0
      g.pop             # FIX: lame
      g.push_local 0
      g.dup
      g.git t
      g.pop

      g.push 1

      g.set_local 0

      t.set!
    end
  end
end
