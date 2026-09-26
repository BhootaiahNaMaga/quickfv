# M3's vacuity set, loaded as a session.
analyze -sv12 ../m0_fifo/rtl/fifo.sv ../m3_vacuity/fifo_vacuity.sv
elaborate -top fifo
clock clk
reset -expression {!reset_}
