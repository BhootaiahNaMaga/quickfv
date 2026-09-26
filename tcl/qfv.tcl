# QuickFV Tcl package (SPEC section 7.3).
#
# Keeps one `qfv serve` process open through a pipe, so the design is loaded
# once and every check reuses it. Works in tclsh 8.5+ and in any EDA tool's
# Tcl shell (including JasperGold's), since it only needs `open |cmd`.
#
#   source qfv.tcl              ;# or: lappend auto_path <dir>; package require qfv
#   qfv::start                  ;# QFV_BIN overrides the qfv executable
#   qfv::load setup.tcl         ;# JasperGold-style setup file
#   set r [qfv::check_assertion {wr_push |=> !fifo_empty} -module fifo_tb -disable {tb_reset}]
#   qfv::verdict $r a1          ;# -> CEX / PASS_BOUNDED / VACUOUS / ...
#   qfv::export_jg handoff.tcl
#   qfv::stop
#
# Every command returns the server's JSON result as a string.

package provide qfv 0.4

namespace eval qfv {
    variable chan ""
    variable id 0
}

proc qfv::start {{work qfv_session}} {
    variable chan
    set bin qfv
    if {[info exists ::env(QFV_BIN)]} { set bin $::env(QFV_BIN) }
    # A list, not a string: paths with spaces stay single arguments.
    set chan [open |[list $bin serve --work $work] r+]
    fconfigure $chan -buffering line -translation lf
    return $chan
}

proc qfv::stop {} {
    variable chan
    if {$chan ne ""} { catch {close $chan}; set chan "" }
}

# JSON string literal for a Tcl string.
proc qfv::str {s} {
    return "\"[string map [list \\ \\\\ \" \\\" \n \\n \r \\r \t \\t] $s]\""
}

# Sends one request; streamed events are skipped; returns the result JSON.
proc qfv::call {method params} {
    variable chan
    variable id
    if {$chan eq ""} { error "qfv: not started (call qfv::start)" }
    incr id
    puts $chan "{\"id\":$id,\"method\":[qfv::str $method],\"params\":$params}"
    while {[gets $chan line] >= 0} {
        if {[string first "\"result\":" $line] >= 0 && [string first "\"id\":$id," $line] >= 0} {
            # Strip the {"id":N,"result": ... } envelope.
            set start [expr {[string first "\"result\":" $line] + 9}]
            return [string range $line $start end-1]
        }
    }
    error "qfv: server closed the connection"
}

proc qfv::load {setup} {
    return [qfv::call load "{\"setup_file\":[qfv::str [file normalize $setup]]}"]
}

# qfv::check_assertion SVA ?-module M? ?-name N? ?-disable EXPR? ?-budget SECONDS?
proc qfv::check_assertion {sva args} {
    array set o {-module "" -name "" -disable "" -budget 30}
    array set o $args
    set p "{\"sva\":[qfv::str $sva],\"module\":[qfv::str $o(-module)],\"name\":[qfv::str $o(-name)]"
    append p ",\"disable_iff\":[qfv::str $o(-disable)],\"budget_s\":$o(-budget)}"
    return [qfv::call check_assertion $p]
}

proc qfv::assume {sva args} {
    array set o {-module "" -name ""}
    array set o $args
    return [qfv::call add_assumption "{\"sva\":[qfv::str $sva],\"module\":[qfv::str $o(-module)],\"name\":[qfv::str $o(-name)]}"]
}

proc qfv::check_all {{budget 30}} { return [qfv::call check_all "{\"budget_s\":$budget}"] }
proc qfv::list_assertions {} { return [qfv::call list_assertions "{}"] }
proc qfv::remove {id} { return [qfv::call remove "{\"id\":[qfv::str $id]}"] }
proc qfv::trace {id} { return [qfv::call get_trace "{\"id\":[qfv::str $id]}"] }
proc qfv::export_jg {path} {
    return [qfv::call export_jaspergold "{\"path\":[qfv::str [file normalize $path]]}"]
}

# Extracts one assertion's verdict from a result JSON ("" if absent).
proc qfv::verdict {json id} {
    if {[regexp "\"verdicts\":\\{\[^\\}\]*\"[string map {# \\#} $id]\":\"(\[A-Z_\]+)\"" $json -> v]} {
        return $v
    }
    return ""
}
