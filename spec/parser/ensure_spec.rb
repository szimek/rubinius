require File.dirname(__FILE__) + '/../spec_helper'

describe "An Ensure node" do
  relates <<-ruby do
      begin
        # do nothing
      rescue
        # do nothing
      ensure
        # do nothing
      end
    ruby

    parse do
      [:ensure, [:rescue, [:resbody, [:array], nil]], [:nil]]
    end

    compile do |g|
      top    = g.new_label
      dunno  = g.new_label
      bottom = g.new_label

      top.set!

      g.push_modifiers
      g.push :nil
      g.pop_modifiers
      g.goto bottom

      dunno.set!

      g.push :nil
      g.pop

      g.push_exception
      g.raise_exc

      bottom.set!

      g.push :nil
      g.pop
    end
  end

  relates <<-ruby do
      begin
        (1 + 1)
      rescue SyntaxError => e1
        2
      rescue Exception => e2
        3
      else
        4
      ensure
        5
      end
    ruby

    parse do
      [:ensure,
         [:rescue,
          [:call, [:lit, 1], :+, [:arglist, [:lit, 1]]],
          [:resbody,
           [:array, [:const, :SyntaxError], [:lasgn, :e1, [:gvar, :$!]]],
           [:lit, 2]],
          [:resbody,
           [:array, [:const, :Exception], [:lasgn, :e2, [:gvar, :$!]]],
           [:lit, 3]],
          [:lit, 4]],
         [:lit, 5]]
    end

    compile do |g|
      jump_top = g.new_label
      jump_top.set!

      in_rescue :SyntaxError, :Exception, :ensure, 2 do |section|
        case section
        when :body then
          g.push 1
          g.push 1
          g.meta_send_op_plus
        when :SyntaxError then
          g.push_exception
          g.set_local 0
          g.push 2
        when :Exception then
          g.push_exception
          g.set_local 1
          g.push 3
        when :else then
          g.pop         # TODO: should this be built in?
          g.push 4
        when :ensure then
          g.push 5
          g.pop
        end
      end
    end
  end

  relates <<-ruby do
      begin
        a
      rescue
        # do nothing
      ensure
        # do nothing
      end
    ruby

    parse do
      [:ensure,
         [:rescue, [:call, nil, :a, [:arglist]], [:resbody, [:array], nil]],
         [:nil]]
    end

    compile do |g|
      in_rescue :StandardError, :ensure do |section|
        case section
        when :body then
          g.push :self
          g.send :a, 0, true
        when :StandardError then
          g.push :nil
        when :ensure then
          g.push :nil
          g.pop
        end
      end
    end
  end

  relates <<-ruby do
      begin
        a
      rescue => mes
        # do nothing
      end

      begin
        b
      rescue => mes
        # do nothing
      end
    ruby

    parse do
      [:block,
         [:rescue,
          [:call, nil, :a, [:arglist]],
          [:resbody, [:array, [:lasgn, :mes, [:gvar, :$!]]], nil]],
         [:rescue,
          [:call, nil, :b, [:arglist]],
          [:resbody, [:array, [:lasgn, :mes, [:gvar, :$!]]], nil]]]
    end

    compile do |g|
      in_rescue :StandardError, 1 do |section|
        case section
        when :body then
          g.push :self
          g.send :a, 0, true
        when :StandardError then
          g.push_exception
          g.set_local 0
          g.push :nil
        end
      end

      g.pop

      in_rescue :StandardError, 2 do |section|
        case section
        when :body then
          g.push :self
          g.send :b, 0, true
        when :StandardError then
          g.push_exception
          g.set_local 0
          g.push :nil
        end
      end
    end
  end
end
