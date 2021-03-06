describe :time_local, :shared => true do
  it "creates a time based on given values, interpreted in the local time zone" do
    with_timezone("PST", -8) do
      Time.send(@method, 2000, "jan", 1, 20, 15, 1).to_a.should ==
        [1, 15, 20, 1, 1, 2000, 6, 1, false, "PST"]
    end
  end

  it "respects rare old timezones" do
    with_timezone("Europe/Amsterdam") do
      Time.send(@method, 1910, 1, 1).to_a.should ==
        [0, 0, 0, 1, 1, 1910, 6, 1, false, "AMT"]
    end
  end

  it "creates a time based on given C-style gmtime arguments, interpreted in the local time zone" do
    with_timezone("PST", -8) do
      Time.send(@method, 1, 15, 20, 1, 1, 2000, :ignored, :ignored, :ignored, :ignored).to_a.should ==
        [1, 15, 20, 1, 1, 2000, 6, 1, false, "PST"]
    end
  end
end
