/*
 * Copyright (c) 2015 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "command-line.h"
#include <errno.h>
#include <getopt.h>
#include <sys/wait.h>
#include "dynamic-string.h"
#include "fatal-signal.h"
#include "match.h"
#include "ofp-actions.h"
#include "ofpbuf.h"
#include "ovn/lib/actions.h"
#include "ovn/lib/expr.h"
#include "ovn/lib/lex.h"
#include "ovs-thread.h"
#include "ovstest.h"
#include "shash.h"
#include "simap.h"
#include "util.h"
#include "openvswitch/vlog.h"

/* --relops: Bitmap of the relational operators to test, in exhaustive test. */
static unsigned int test_relops;

/* --nvars: Number of numeric variables to test, in exhaustive test. */
static int test_nvars = 2;

/* --svars: Number of string variables to test, in exhaustive test. */
static int test_svars = 2;

/* --bits: Number of bits per variable, in exhaustive test. */
static int test_bits = 3;

/* --operation: The operation to test, in exhaustive test. */
static enum { OP_CONVERT, OP_SIMPLIFY, OP_NORMALIZE, OP_FLOW } operation
    = OP_FLOW;

/* --parallel: Number of parallel processes to use in test. */
static int test_parallel = 1;

/* -m, --more: Message verbosity */
static int verbosity;

static void
compare_token(const struct lex_token *a, const struct lex_token *b)
{
    if (a->type != b->type) {
        fprintf(stderr, "type differs: %d -> %d\n", a->type, b->type);
        return;
    }

    if (!((a->s && b->s && !strcmp(a->s, b->s))
          || (!a->s && !b->s))) {
        fprintf(stderr, "string differs: %s -> %s\n",
                a->s ? a->s : "(null)",
                b->s ? b->s : "(null)");
        return;
    }

    if (a->type == LEX_T_INTEGER || a->type == LEX_T_MASKED_INTEGER) {
        if (memcmp(&a->value, &b->value, sizeof a->value)) {
            fprintf(stderr, "value differs\n");
            return;
        }

        if (a->type == LEX_T_MASKED_INTEGER
            && memcmp(&a->mask, &b->mask, sizeof a->mask)) {
            fprintf(stderr, "mask differs\n");
            return;
        }

        if (a->format != b->format
            && !(a->format == LEX_F_HEXADECIMAL
                 && b->format == LEX_F_DECIMAL
                 && a->value.integer == 0)) {
            fprintf(stderr, "format differs: %d -> %d\n",
                    a->format, b->format);
        }
    }
}

static void
test_lex(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct ds input;
    struct ds output;

    ds_init(&input);
    ds_init(&output);
    while (!ds_get_test_line(&input, stdin)) {
        struct lexer lexer;

        lexer_init(&lexer, ds_cstr(&input));
        ds_clear(&output);
        while (lexer_get(&lexer) != LEX_T_END) {
            size_t len = output.length;
            lex_token_format(&lexer.token, &output);

            /* Check that the formatted version can really be parsed back
             * losslessly. */
            if (lexer.token.type != LEX_T_ERROR) {
                const char *s = ds_cstr(&output) + len;
                struct lexer l2;

                lexer_init(&l2, s);
                lexer_get(&l2);
                compare_token(&lexer.token, &l2.token);
                lexer_destroy(&l2);
            }
            ds_put_char(&output, ' ');
        }
        lexer_destroy(&lexer);

        ds_chomp(&output, ' ');
        puts(ds_cstr(&output));
    }
    ds_destroy(&input);
    ds_destroy(&output);
}

static void
create_symtab(struct shash *symtab)
{
    shash_init(symtab);

    /* Reserve a pair of registers for the logical inport and outport.  A full
     * 32-bit register each is bigger than we need, but the expression code
     * doesn't yet support string fields that occupy less than a full OXM. */
    expr_symtab_add_string(symtab, "inport", MFF_REG6, NULL);
    expr_symtab_add_string(symtab, "outport", MFF_REG7, NULL);

    expr_symtab_add_field(symtab, "xreg0", MFF_XREG0, NULL, false);
    expr_symtab_add_field(symtab, "xreg1", MFF_XREG1, NULL, false);
    expr_symtab_add_field(symtab, "xreg2", MFF_XREG2, NULL, false);

    expr_symtab_add_subfield(symtab, "reg0", NULL, "xreg0[32..63]");
    expr_symtab_add_subfield(symtab, "reg1", NULL, "xreg0[0..31]");
    expr_symtab_add_subfield(symtab, "reg2", NULL, "xreg1[32..63]");
    expr_symtab_add_subfield(symtab, "reg3", NULL, "xreg1[0..31]");
    expr_symtab_add_subfield(symtab, "reg4", NULL, "xreg2[32..63]");
    expr_symtab_add_subfield(symtab, "reg5", NULL, "xreg2[0..31]");

    expr_symtab_add_field(symtab, "eth.src", MFF_ETH_SRC, NULL, false);
    expr_symtab_add_field(symtab, "eth.dst", MFF_ETH_DST, NULL, false);
    expr_symtab_add_field(symtab, "eth.type", MFF_ETH_TYPE, NULL, true);

    expr_symtab_add_field(symtab, "vlan.tci", MFF_VLAN_TCI, NULL, false);
    expr_symtab_add_predicate(symtab, "vlan.present", "vlan.tci[12]");
    expr_symtab_add_subfield(symtab, "vlan.pcp", "vlan.present",
                             "vlan.tci[13..15]");
    expr_symtab_add_subfield(symtab, "vlan.vid", "vlan.present",
                             "vlan.tci[0..11]");

    expr_symtab_add_predicate(symtab, "ip4", "eth.type == 0x800");
    expr_symtab_add_predicate(symtab, "ip6", "eth.type == 0x86dd");
    expr_symtab_add_predicate(symtab, "ip", "ip4 || ip6");
    expr_symtab_add_field(symtab, "ip.proto", MFF_IP_PROTO, "ip", true);
    expr_symtab_add_field(symtab, "ip.dscp", MFF_IP_DSCP, "ip", false);
    expr_symtab_add_field(symtab, "ip.ecn", MFF_IP_ECN, "ip", false);
    expr_symtab_add_field(symtab, "ip.ttl", MFF_IP_TTL, "ip", false);

    expr_symtab_add_field(symtab, "ip4.src", MFF_IPV4_SRC, "ip4", false);
    expr_symtab_add_field(symtab, "ip4.dst", MFF_IPV4_DST, "ip4", false);

    expr_symtab_add_predicate(symtab, "icmp4", "ip4 && ip.proto == 1");
    expr_symtab_add_field(symtab, "icmp4.type", MFF_ICMPV4_TYPE, "icmp4",
              false);
    expr_symtab_add_field(symtab, "icmp4.code", MFF_ICMPV4_CODE, "icmp4",
              false);

    expr_symtab_add_field(symtab, "ip6.src", MFF_IPV6_SRC, "ip6", false);
    expr_symtab_add_field(symtab, "ip6.dst", MFF_IPV6_DST, "ip6", false);
    expr_symtab_add_field(symtab, "ip6.label", MFF_IPV6_LABEL, "ip6", false);

    expr_symtab_add_predicate(symtab, "icmp6", "ip6 && ip.proto == 58");
    expr_symtab_add_field(symtab, "icmp6.type", MFF_ICMPV6_TYPE, "icmp6",
                          true);
    expr_symtab_add_field(symtab, "icmp6.code", MFF_ICMPV6_CODE, "icmp6",
                          true);

    expr_symtab_add_predicate(symtab, "icmp", "icmp4 || icmp6");

    expr_symtab_add_field(symtab, "ip.frag", MFF_IP_FRAG, "ip", false);
    expr_symtab_add_predicate(symtab, "ip.is_frag", "ip.frag[0]");
    expr_symtab_add_predicate(symtab, "ip.later_frag", "ip.frag[1]");
    expr_symtab_add_predicate(symtab, "ip.first_frag", "ip.is_frag && !ip.later_frag");

    expr_symtab_add_predicate(symtab, "arp", "eth.type == 0x806");
    expr_symtab_add_field(symtab, "arp.op", MFF_ARP_OP, "arp", false);
    expr_symtab_add_field(symtab, "arp.spa", MFF_ARP_SPA, "arp", false);
    expr_symtab_add_field(symtab, "arp.sha", MFF_ARP_SHA, "arp", false);
    expr_symtab_add_field(symtab, "arp.tpa", MFF_ARP_TPA, "arp", false);
    expr_symtab_add_field(symtab, "arp.tha", MFF_ARP_THA, "arp", false);

    expr_symtab_add_predicate(symtab, "nd", "icmp6.type == {135, 136} && icmp6.code == 0");
    expr_symtab_add_field(symtab, "nd.target", MFF_ND_TARGET, "nd", false);
    expr_symtab_add_field(symtab, "nd.sll", MFF_ND_SLL,
              "nd && icmp6.type == 135", false);
    expr_symtab_add_field(symtab, "nd.tll", MFF_ND_TLL,
              "nd && icmp6.type == 136", false);

    expr_symtab_add_predicate(symtab, "tcp", "ip.proto == 6");
    expr_symtab_add_field(symtab, "tcp.src", MFF_TCP_SRC, "tcp", false);
    expr_symtab_add_field(symtab, "tcp.dst", MFF_TCP_DST, "tcp", false);
    expr_symtab_add_field(symtab, "tcp.flags", MFF_TCP_FLAGS, "tcp", false);

    expr_symtab_add_predicate(symtab, "udp", "ip.proto == 17");
    expr_symtab_add_field(symtab, "udp.src", MFF_UDP_SRC, "udp", false);
    expr_symtab_add_field(symtab, "udp.dst", MFF_UDP_DST, "udp", false);

    expr_symtab_add_predicate(symtab, "sctp", "ip.proto == 132");
    expr_symtab_add_field(symtab, "sctp.src", MFF_SCTP_SRC, "sctp", false);
    expr_symtab_add_field(symtab, "sctp.dst", MFF_SCTP_DST, "sctp", false);

    /* For negative testing. */
    expr_symtab_add_field(symtab, "bad_prereq", MFF_XREG0, "xyzzy", false);
    expr_symtab_add_field(symtab, "self_recurse", MFF_XREG0,
                          "self_recurse != 0", false);
    expr_symtab_add_field(symtab, "mutual_recurse_1", MFF_XREG0,
                          "mutual_recurse_2 != 0", false);
    expr_symtab_add_field(symtab, "mutual_recurse_2", MFF_XREG0,
                          "mutual_recurse_1 != 0", false);
    expr_symtab_add_string(symtab, "big_string", MFF_XREG0, NULL);
}

static void
test_parse_expr__(int steps)
{
    struct shash symtab;
    struct simap ports;
    struct ds input;

    create_symtab(&symtab);

    simap_init(&ports);
    simap_put(&ports, "eth0", 5);
    simap_put(&ports, "eth1", 6);
    simap_put(&ports, "LOCAL", ofp_to_u16(OFPP_LOCAL));

    ds_init(&input);
    while (!ds_get_test_line(&input, stdin)) {
        struct expr *expr;
        char *error;

        expr = expr_parse_string(ds_cstr(&input), &symtab, &error);
        if (!error && steps > 0) {
            expr = expr_annotate(expr, &symtab, &error);
        }
        if (!error) {
            if (steps > 1) {
                expr = expr_simplify(expr);
            }
            if (steps > 2) {
                expr = expr_normalize(expr);
                ovs_assert(expr_is_normalized(expr));
            }
        }
        if (!error) {
            if (steps > 3) {
                struct hmap matches;

                expr_to_matches(expr, &ports, &matches);
                expr_matches_print(&matches, stdout);
                expr_matches_destroy(&matches);
            } else {
                struct ds output = DS_EMPTY_INITIALIZER;
                expr_format(expr, &output);
                puts(ds_cstr(&output));
                ds_destroy(&output);
            }
        } else {
            puts(error);
            free(error);
        }
        expr_destroy(expr);
    }
    ds_destroy(&input);

    simap_destroy(&ports);
    expr_symtab_destroy(&symtab);
    shash_destroy(&symtab);
}

static void
test_parse_expr(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    test_parse_expr__(0);
}

static void
test_annotate_expr(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    test_parse_expr__(1);
}

static void
test_simplify_expr(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    test_parse_expr__(2);
}

static void
test_normalize_expr(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    test_parse_expr__(3);
}

static void
test_expr_to_flows(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    test_parse_expr__(4);
}

/* Evaluate an expression. */

static bool evaluate_expr(const struct expr *, unsigned int subst, int n_bits);

static bool
evaluate_andor_expr(const struct expr *expr, unsigned int subst, int n_bits,
                    bool short_circuit)
{
    const struct expr *sub;

    LIST_FOR_EACH (sub, node, &expr->andor) {
        if (evaluate_expr(sub, subst, n_bits) == short_circuit) {
            return short_circuit;
        }
    }
    return !short_circuit;
}

static bool
evaluate_cmp_expr(const struct expr *expr, unsigned int subst, int n_bits)
{
    int var_idx = atoi(expr->cmp.symbol->name + 1);
    if (expr->cmp.symbol->name[0] == 'n') {
        unsigned var_mask = (1u << n_bits) - 1;
        unsigned int arg1 = (subst >> (var_idx * n_bits)) & var_mask;
        unsigned int arg2 = ntohll(expr->cmp.value.integer);
        unsigned int mask = ntohll(expr->cmp.mask.integer);

        ovs_assert(!(mask & ~var_mask));
        ovs_assert(!(arg2 & ~var_mask));
        ovs_assert(!(arg2 & ~mask));

        arg1 &= mask;
        switch (expr->cmp.relop) {
        case EXPR_R_EQ:
            return arg1 == arg2;

        case EXPR_R_NE:
            return arg1 != arg2;

        case EXPR_R_LT:
            return arg1 < arg2;

        case EXPR_R_LE:
            return arg1 <= arg2;

        case EXPR_R_GT:
            return arg1 > arg2;

        case EXPR_R_GE:
            return arg1 >= arg2;

        default:
            OVS_NOT_REACHED();
        }
    } else if (expr->cmp.symbol->name[0] == 's') {
        unsigned int arg1 = (subst >> (test_nvars * n_bits + var_idx)) & 1;
        unsigned int arg2 = atoi(expr->cmp.string);
        return arg1 == arg2;
    } else {
        OVS_NOT_REACHED();
    }
}

/* Evaluates 'expr' and returns its Boolean result.  'subst' provides the value
 * for the variables, which must be 'n_bits' bits each and be named "a", "b",
 * "c", etc.  The value of variable "a" is the least-significant 'n_bits' bits
 * of 'subst', the value of "b" is the next 'n_bits' bits, and so on. */
static bool
evaluate_expr(const struct expr *expr, unsigned int subst, int n_bits)
{
    switch (expr->type) {
    case EXPR_T_CMP:
        return evaluate_cmp_expr(expr, subst, n_bits);

    case EXPR_T_AND:
        return evaluate_andor_expr(expr, subst, n_bits, false);

    case EXPR_T_OR:
        return evaluate_andor_expr(expr, subst, n_bits, true);

    case EXPR_T_BOOLEAN:
        return expr->boolean;

    default:
        OVS_NOT_REACHED();
    }
}

static void
test_evaluate_expr(struct ovs_cmdl_context *ctx)
{
    int a = atoi(ctx->argv[1]);
    int b = atoi(ctx->argv[2]);
    int c = atoi(ctx->argv[3]);
    unsigned int subst = a | (b << 3) || (c << 6);
    struct shash symtab;
    struct ds input;

    shash_init(&symtab);
    expr_symtab_add_field(&symtab, "xreg0", MFF_XREG0, NULL, false);
    expr_symtab_add_field(&symtab, "xreg1", MFF_XREG1, NULL, false);
    expr_symtab_add_field(&symtab, "xreg2", MFF_XREG1, NULL, false);
    expr_symtab_add_subfield(&symtab, "a", NULL, "xreg0[0..2]");
    expr_symtab_add_subfield(&symtab, "b", NULL, "xreg1[0..2]");
    expr_symtab_add_subfield(&symtab, "c", NULL, "xreg2[0..2]");

    ds_init(&input);
    while (!ds_get_test_line(&input, stdin)) {
        struct expr *expr;
        char *error;

        expr = expr_parse_string(ds_cstr(&input), &symtab, &error);
        if (!error) {
            expr = expr_annotate(expr, &symtab, &error);
        }
        if (!error) {
            printf("%d\n", evaluate_expr(expr, subst, 3));
        } else {
            puts(error);
            free(error);
        }
        expr_destroy(expr);
    }
    ds_destroy(&input);

    expr_symtab_destroy(&symtab);
    shash_destroy(&symtab);
}

/* Compositions.
 *
 * The "compositions" of a positive integer N are all of the ways that one can
 * add up positive integers to sum to N.  For example, the compositions of 3
 * are 3, 2+1, 1+2, and 1+1+1.
 *
 * We use compositions to find all the ways to break up N terms of a Boolean
 * expression into subexpressions.  Suppose we want to generate all expressions
 * with 3 terms.  The compositions of 3 (ignoring 3 itself) provide the
 * possibilities (x && x) || x, x || (x && x), and x || x || x.  (Of course one
 * can exchange && for || in each case.)  One must recursively compose the
 * sub-expressions whose values are 3 or greater; that is what the "tree shape"
 * concept later covers.
 *
 * To iterate through all compositions of, e.g., 5:
 *
 *     unsigned int state;
 *     int s[5];
 *     int n;
 *
 *     for (n = first_composition(ARRAY_SIZE(s), &state, s); n > 0;
 *          n = next_composition(&state, s, n)) {
 *          // Do something with composition 's' with 'n' elements.
 *     }
 *
 * Algorithm from D. E. Knuth, _The Art of Computer Programming, Vol. 4A:
 * Combinatorial Algorithms, Part 1_, section 7.2.1.1, answer to exercise
 * 12(a).
 */

/* Begins iteration through the compositions of 'n'.  Initializes 's' to the
 * number of elements in the first composition of 'n' and returns that number
 * of elements.  The first composition in fact is always 'n' itself, so the
 * return value will be 1.
 *
 * Initializes '*state' to some internal state information.  The caller must
 * maintain this state (and 's') for use by next_composition().
 *
 * 's' must have room for at least 'n' elements. */
static int
first_composition(int n, unsigned int *state, int s[])
{
    *state = 0;
    s[0] = n;
    return 1;
}

/* Advances 's', with 'sn' elements, to the next composition and returns the
 * number of elements in this new composition, or 0 if no compositions are
 * left.  'state' is the same internal state passed to first_composition(). */
static int
next_composition(unsigned int *state, int s[], int sn)
{
    int j = sn - 1;
    if (++*state & 1) {
        if (s[j] > 1) {
            s[j]--;
            s[j + 1] = 1;
            j++;
        } else {
            j--;
            s[j]++;
        }
    } else {
        if (s[j - 1] > 1) {
            s[j - 1]--;
            s[j + 1] = s[j];
            s[j] = 1;
            j++;
        } else {
            j--;
            s[j] = s[j + 1];
            s[j - 1]++;
            if (!j) {
                return 0;
            }
        }
    }
    return j + 1;
}

static void
test_composition(struct ovs_cmdl_context *ctx)
{
    int n = atoi(ctx->argv[1]);
    unsigned int state;
    int s[50];

    for (int sn = first_composition(n, &state, s); sn;
         sn = next_composition(&state, s, sn)) {
        for (int i = 0; i < sn; i++) {
            printf("%d%c", s[i], i == sn - 1 ? '\n' : ' ');
        }
    }
}

/* Tree shapes.
 *
 * This code generates all possible Boolean expressions with a specified number
 * of terms N (equivalent to the number of external nodes in a tree).
 *
 * See test_tree_shape() for a simple example. */

/* An array of these structures describes the shape of a tree.
 *
 * A single element of struct tree_shape describes a single node in the tree.
 * The node has 'sn' direct children.  From left to right, for i in 0...sn-1,
 * s[i] is 1 if the child is a leaf node, otherwise the child is a subtree and
 * s[i] is the number of leaf nodes within that subtree.  In the latter case,
 * the subtree is described by another struct tree_shape within the enclosing
 * array.  The tree_shapes are ordered in the array in in-order.
 */
struct tree_shape {
    unsigned int state;
    int s[50];
    int sn;
};

static int
init_tree_shape__(struct tree_shape ts[], int n)
{
    if (n <= 2) {
        return 0;
    }

    int n_tses = 1;
    /* Skip the first composition intentionally. */
    ts->sn = first_composition(n, &ts->state, ts->s);
    ts->sn = next_composition(&ts->state, ts->s, ts->sn);
    for (int i = 0; i < ts->sn; i++) {
        n_tses += init_tree_shape__(&ts[n_tses], ts->s[i]);
    }
    return n_tses;
}

/* Initializes 'ts[]' as the first in the set of all of possible shapes of
 * trees with 'n' leaves.  Returns the number of "struct tree_shape"s in the
 * first tree shape. */
static int
init_tree_shape(struct tree_shape ts[], int n)
{
    switch (n) {
    case 1:
        ts->sn = 1;
        ts->s[0] = 1;
        return 1;
    case 2:
        ts->sn = 2;
        ts->s[0] = 1;
        ts->s[1] = 1;
        return 1;
    default:
        return init_tree_shape__(ts, n);
    }
}

/* Advances 'ts', which currently has 'n_tses' elements, to the next possible
 * tree shape with the number of leaves passed to init_tree_shape().  Returns
 * the number of "struct tree_shape"s in the next shape, or 0 if all tree
 * shapes have been visited. */
static int
next_tree_shape(struct tree_shape ts[], int n_tses)
{
    if (n_tses == 1 && ts->sn == 2 && ts->s[0] == 1 && ts->s[1] == 1) {
        return 0;
    }
    while (n_tses > 0) {
        struct tree_shape *p = &ts[n_tses - 1];
        p->sn = p->sn > 1 ? next_composition(&p->state, p->s, p->sn) : 0;
        if (p->sn) {
            for (int i = 0; i < p->sn; i++) {
                n_tses += init_tree_shape__(&ts[n_tses], p->s[i]);
            }
            break;
        }
        n_tses--;
    }
    return n_tses;
}

static void
print_tree_shape(const struct tree_shape ts[], int n_tses)
{
    for (int i = 0; i < n_tses; i++) {
        if (i) {
            printf(", ");
        }
        for (int j = 0; j < ts[i].sn; j++) {
            int k = ts[i].s[j];
            if (k > 9) {
                printf("(%d)", k);
            } else {
                printf("%d", k);
            }
        }
    }
}

static void
test_tree_shape(struct ovs_cmdl_context *ctx)
{
    int n = atoi(ctx->argv[1]);
    struct tree_shape ts[50];
    int n_tses;

    for (n_tses = init_tree_shape(ts, n); n_tses;
         n_tses = next_tree_shape(ts, n_tses)) {
        print_tree_shape(ts, n_tses);
        putchar('\n');
    }
}

/* Iteration through all possible terminal expressions (e.g. EXPR_T_CMP and
 * EXPR_T_BOOLEAN expressions).
 *
 * Given a tree shape, this allows the code to try all possible ways to plug in
 * terms.
 *
 * Example use:
 *
 *     struct expr terminal;
 *     const struct expr_symbol *vars = ...;
 *     int n_vars = ...;
 *     int n_bits = ...;
 *
 *     init_terminal(&terminal, vars[0]);
 *     do {
 *         // Something with 'terminal'.
 *     } while (next_terminal(&terminal, vars, n_vars, n_bits));
 */

/* Sets 'expr' to the first possible terminal expression.  'var' should be the
 * first variable in the ones to be tested. */
static void
init_terminal(struct expr *expr, int phase,
              const struct expr_symbol *nvars[], int n_nvars,
              const struct expr_symbol *svars[], int n_svars)
{
    if (phase < 1 && n_nvars) {
        expr->type = EXPR_T_CMP;
        expr->cmp.symbol = nvars[0];
        expr->cmp.relop = rightmost_1bit_idx(test_relops);
        memset(&expr->cmp.value, 0, sizeof expr->cmp.value);
        memset(&expr->cmp.mask, 0, sizeof expr->cmp.mask);
        expr->cmp.value.integer = htonll(0);
        expr->cmp.mask.integer = htonll(1);
        return;
    }

    if (phase < 2 && n_svars) {
        expr->type = EXPR_T_CMP;
        expr->cmp.symbol = svars[0];
        expr->cmp.relop = EXPR_R_EQ;
        expr->cmp.string = xstrdup("0");
        return;
    }

    expr->type = EXPR_T_BOOLEAN;
    expr->boolean = false;
}

/* Returns 'x' with the rightmost contiguous string of 1s changed to 0s,
 * e.g. 01011100 => 01000000.  See H. S. Warren, Jr., _Hacker's Delight_, 2nd
 * ed., section 2-1. */
static unsigned int
turn_off_rightmost_1s(unsigned int x)
{
    return ((x & -x) + x) & x;
}

static const struct expr_symbol *
next_var(const struct expr_symbol *symbol,
         const struct expr_symbol *vars[], int n_vars)
{
    for (int i = 0; i < n_vars; i++) {
        if (symbol == vars[i]) {
            return i + 1 >= n_vars ? NULL : vars[i + 1];
        }
    }
    OVS_NOT_REACHED();
}

static enum expr_relop
next_relop(enum expr_relop relop)
{
    unsigned int remaining_relops = test_relops & ~((1u << (relop + 1)) - 1);
    return (remaining_relops
            ? rightmost_1bit_idx(remaining_relops)
            : rightmost_1bit_idx(test_relops));
}

/* Advances 'expr' to the next possible terminal expression within the 'n_vars'
 * variables of 'n_bits' bits each in 'vars[]'. */
static bool
next_terminal(struct expr *expr,
              const struct expr_symbol *nvars[], int n_nvars, int n_bits,
              const struct expr_symbol *svars[], int n_svars)
{
    if (expr->type == EXPR_T_BOOLEAN) {
        if (expr->boolean) {
            return false;
        } else {
            expr->boolean = true;
            return true;
        }
    }

    if (!expr->cmp.symbol->width) {
        int next_value = atoi(expr->cmp.string) + 1;
        free(expr->cmp.string);
        if (next_value > 1) {
            expr->cmp.symbol = next_var(expr->cmp.symbol, svars, n_svars);
            if (!expr->cmp.symbol) {
                init_terminal(expr, 2, nvars, n_nvars, svars, n_svars);
                return true;
            }
            next_value = 0;
        }
        expr->cmp.string = xasprintf("%d", next_value);
        return true;
    }

    unsigned int next;

    next = (ntohll(expr->cmp.value.integer)
            + (ntohll(expr->cmp.mask.integer) << n_bits));
    for (;;) {
        next++;
        unsigned m = next >> n_bits;
        unsigned v = next & ((1u << n_bits) - 1);
        if (next >= (1u << (2 * n_bits))) {
            enum expr_relop old_relop = expr->cmp.relop;
            expr->cmp.relop = next_relop(old_relop);
            if (expr->cmp.relop <= old_relop) {
                expr->cmp.symbol = next_var(expr->cmp.symbol, nvars, n_nvars);
                if (!expr->cmp.symbol) {
                    init_terminal(expr, 1, nvars, n_nvars, svars, n_svars);
                    return true;
                }
            }
            next = 0;
        } else if (m == 0) {
            /* Skip: empty mask is pathological. */
        } else if (v & ~m) {
            /* Skip: 1-bits in value correspond to 0-bits in mask. */
        } else if (turn_off_rightmost_1s(m)
                   && (expr->cmp.relop != EXPR_R_EQ &&
                       expr->cmp.relop != EXPR_R_NE)) {
            /* Skip: can't have discontiguous mask for > >= < <=. */
        } else {
            expr->cmp.value.integer = htonll(v);
            expr->cmp.mask.integer = htonll(m);
            return true;
        }
    }
}

static struct expr *
make_terminal(struct expr ***terminalp)
{
    struct expr *e = expr_create_boolean(true);
    **terminalp = e;
    (*terminalp)++;
    return e;
}

static struct expr *
build_simple_tree(enum expr_type type, int n, struct expr ***terminalp)
{
    if (n == 2) {
        struct expr *e = expr_create_andor(type);
        for (int i = 0; i < 2; i++) {
            struct expr *sub = make_terminal(terminalp);
            list_push_back(&e->andor, &sub->node);
        }
        return e;
    } else if (n == 1) {
        return make_terminal(terminalp);
    } else {
        OVS_NOT_REACHED();
    }
}

static struct expr *
build_tree_shape(enum expr_type type, const struct tree_shape **tsp,
                 struct expr ***terminalp)
{
    const struct tree_shape *ts = *tsp;
    (*tsp)++;

    struct expr *e = expr_create_andor(type);
    enum expr_type t = type == EXPR_T_AND ? EXPR_T_OR : EXPR_T_AND;
    for (int i = 0; i < ts->sn; i++) {
        struct expr *sub = (ts->s[i] > 2
                            ? build_tree_shape(t, tsp, terminalp)
                            : build_simple_tree(t, ts->s[i], terminalp));
        list_push_back(&e->andor, &sub->node);
    }
    return e;
}

struct test_rule {
    struct cls_rule cr;
};

static void
free_rule(struct test_rule *test_rule)
{
    cls_rule_destroy(&test_rule->cr);
    free(test_rule);
}

static int
test_tree_shape_exhaustively(struct expr *expr, struct shash *symtab,
                             struct expr *terminals[], int n_terminals,
                             const struct expr_symbol *nvars[], int n_nvars,
                             int n_bits,
                             const struct expr_symbol *svars[], int n_svars)
{
    struct simap string_map = SIMAP_INITIALIZER(&string_map);
    simap_put(&string_map, "0", 0);
    simap_put(&string_map, "1", 1);

    int n_tested = 0;

    const unsigned int var_mask = (1u << n_bits) - 1;
    for (int i = 0; i < n_terminals; i++) {
        init_terminal(terminals[i], 0, nvars, n_nvars, svars, n_svars);
    }

    struct ds s = DS_EMPTY_INITIALIZER;
    struct flow f;
    memset(&f, 0, sizeof f);
    for (;;) {
        for (int i = n_terminals - 1; ; i--) {
            if (!i) {
                ds_destroy(&s);
                simap_destroy(&string_map);
                return n_tested;
            }
            if (next_terminal(terminals[i], nvars, n_nvars, n_bits,
                              svars, n_svars)) {
                break;
            }
            init_terminal(terminals[i], 0, nvars, n_nvars, svars, n_svars);
        }
        ovs_assert(expr_honors_invariants(expr));

        n_tested++;

        struct expr *modified;
        if (operation == OP_CONVERT) {
            ds_clear(&s);
            expr_format(expr, &s);

            char *error;
            modified = expr_parse_string(ds_cstr(&s), symtab, &error);
            if (error) {
                fprintf(stderr, "%s fails to parse (%s)\n",
                        ds_cstr(&s), error);
                exit(EXIT_FAILURE);
            }
        } else if (operation >= OP_SIMPLIFY) {
            modified  = expr_simplify(expr_clone(expr));
            ovs_assert(expr_honors_invariants(modified));

            if (operation >= OP_NORMALIZE) {
                modified = expr_normalize(modified);
                ovs_assert(expr_is_normalized(modified));
            }
        }

        struct hmap matches;
        struct classifier cls;
        if (operation >= OP_FLOW) {
            struct expr_match *m;
            struct test_rule *test_rule;

            expr_to_matches(modified, &string_map, &matches);

            classifier_init(&cls, NULL);
            HMAP_FOR_EACH (m, hmap_node, &matches) {
                test_rule = xmalloc(sizeof *test_rule);
                cls_rule_init(&test_rule->cr, &m->match, 0);
                classifier_insert(&cls, &test_rule->cr, CLS_MIN_VERSION,
                                  m->conjunctions, m->n);
            }
        }
        for (int subst = 0; subst < 1 << (n_bits * n_nvars + n_svars);
             subst++) {
            bool expected = evaluate_expr(expr, subst, n_bits);
            bool actual = evaluate_expr(modified, subst, n_bits);
            if (actual != expected) {
                struct ds expr_s, modified_s;

                ds_init(&expr_s);
                expr_format(expr, &expr_s);

                ds_init(&modified_s);
                expr_format(modified, &modified_s);

                fprintf(stderr,
                        "%s evaluates to %d, but %s evaluates to %d, for",
                        ds_cstr(&expr_s), expected,
                        ds_cstr(&modified_s), actual);
                for (int i = 0; i < n_nvars; i++) {
                    if (i > 0) {
                        fputs(",", stderr);
                    }
                    fprintf(stderr, " n%d = 0x%x", i,
                            (subst >> (n_bits * i)) & var_mask);
                }
                for (int i = 0; i < n_svars; i++) {
                    fprintf(stderr, ", s%d = \"%d\"", i,
                            (subst >> (n_bits * n_nvars + i)) & 1);
                }
                putc('\n', stderr);
                exit(EXIT_FAILURE);
            }

            if (operation >= OP_FLOW) {
                for (int i = 0; i < n_nvars; i++) {
                    f.regs[i] = (subst >> (i * n_bits)) & var_mask;
                }
                for (int i = 0; i < n_svars; i++) {
                    f.regs[n_nvars + i] = ((subst >> (n_nvars * n_bits + i))
                                           & 1);
                }
                bool found = classifier_lookup(&cls, CLS_MIN_VERSION,
                                               &f, NULL) != NULL;
                if (expected != found) {
                    struct ds expr_s, modified_s;

                    ds_init(&expr_s);
                    expr_format(expr, &expr_s);

                    ds_init(&modified_s);
                    expr_format(modified, &modified_s);

                    fprintf(stderr,
                            "%s and %s evaluate to %d, for",
                            ds_cstr(&expr_s), ds_cstr(&modified_s), expected);
                    for (int i = 0; i < n_nvars; i++) {
                        if (i > 0) {
                            fputs(",", stderr);
                        }
                        fprintf(stderr, " n%d = 0x%x", i,
                                (subst >> (n_bits * i)) & var_mask);
                    }
                    for (int i = 0; i < n_svars; i++) {
                        fprintf(stderr, ", s%d = \"%d\"", i,
                                (subst >> (n_bits * n_nvars + i)) & 1);
                    }
                    fputs(".\n", stderr);

                    fprintf(stderr, "Converted to classifier:\n");
                    expr_matches_print(&matches, stderr);
                    fprintf(stderr,
                            "However, %s flow was found in the classifier.\n",
                            found ? "a" : "no");
                    exit(EXIT_FAILURE);
                }
            }
        }
        if (operation >= OP_FLOW) {
            struct test_rule *test_rule;

            CLS_FOR_EACH (test_rule, cr, &cls) {
                classifier_remove(&cls, &test_rule->cr);
                ovsrcu_postpone(free_rule, test_rule);
            }
            classifier_destroy(&cls);
            ovsrcu_quiesce();

            expr_matches_destroy(&matches);
        }
        expr_destroy(modified);
    }
}

#ifndef _WIN32
#ifndef WAIT_ANY
# define WAIT_ANY  (-1)
#endif
static void
wait_pid(pid_t *pids, int *n)
{
    int status;
    pid_t pid;

    pid = waitpid(WAIT_ANY, &status, 0);
    if (pid < 0) {
        ovs_fatal(errno, "waitpid failed");
    } else if (WIFEXITED(status)) {
        if (WEXITSTATUS(status)) {
            exit(WEXITSTATUS(status));
        }
    } else if (WIFSIGNALED(status)) {
        raise(WTERMSIG(status));
        exit(1);
    } else {
        OVS_NOT_REACHED();
    }

    for (int i = 0; i < *n; i++) {
        if (pids[i] == pid) {
            pids[i] = pids[--*n];
            return;
        }
    }
    ovs_fatal(0, "waitpid returned unknown child");
}
#endif

static void
test_exhaustive(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    int n_terminals = atoi(ctx->argv[1]);
    struct tree_shape ts[50];
    int n_tses;

    struct shash symtab;
    const struct expr_symbol *nvars[4];
    const struct expr_symbol *svars[4];

    ovs_assert(test_nvars <= ARRAY_SIZE(nvars));
    ovs_assert(test_svars <= ARRAY_SIZE(svars));
    ovs_assert(test_nvars + test_svars <= FLOW_N_REGS);

    shash_init(&symtab);
    for (int i = 0; i < test_nvars; i++) {
        char *name = xasprintf("n%d", i);
        nvars[i] = expr_symtab_add_field(&symtab, name, MFF_REG0 + i, NULL,
                                         false);
        free(name);
    }
    for (int i = 0; i < test_svars; i++) {
        char *name = xasprintf("s%d", i);
        svars[i] = expr_symtab_add_string(&symtab, name,
                                          MFF_REG0 + test_nvars + i, NULL);
        free(name);
    }

#ifndef _WIN32
    pid_t *children = xmalloc(test_parallel * sizeof *children);
    int n_children = 0;
#endif

    int n_tested = 0;
    for (int i = 0; i < 2; i++) {
        enum expr_type base_type = i ? EXPR_T_OR : EXPR_T_AND;

        for (n_tses = init_tree_shape(ts, n_terminals); n_tses;
             n_tses = next_tree_shape(ts, n_tses)) {
            const struct tree_shape *tsp = ts;
            struct expr *terminals[50];
            struct expr **terminalp = terminals;
            struct expr *expr = build_tree_shape(base_type, &tsp, &terminalp);
            ovs_assert(terminalp == &terminals[n_terminals]);

            if (verbosity > 0) {
                print_tree_shape(ts, n_tses);
                printf(": ");
                struct ds s = DS_EMPTY_INITIALIZER;
                expr_format(expr, &s);
                puts(ds_cstr(&s));
                ds_destroy(&s);
            }

#ifndef _WIN32
            if (test_parallel > 1) {
                pid_t pid = xfork();
                if (!pid) {
                    test_tree_shape_exhaustively(expr, &symtab,
                                                 terminals, n_terminals,
                                                 nvars, test_nvars, test_bits,
                                                 svars, test_svars);
                    expr_destroy(expr);
                    exit(0);
                } else {
                    if (n_children >= test_parallel) {
                        wait_pid(children, &n_children);
                    }
                    children[n_children++] = pid;
                }
            } else
#endif
            {
                n_tested += test_tree_shape_exhaustively(
                    expr, &symtab, terminals, n_terminals,
                    nvars, test_nvars, test_bits,
                    svars, test_svars);
            }
            expr_destroy(expr);
        }
    }
#ifndef _WIN32
    while (n_children > 0) {
        wait_pid(children, &n_children);
    }
    free(children);
#endif

    printf("Tested ");
    switch (operation) {
    case OP_CONVERT:
        printf("converting");
        break;
    case OP_SIMPLIFY:
        printf("simplifying");
        break;
    case OP_NORMALIZE:
        printf("normalizing");
        break;
    case OP_FLOW:
        printf("converting to flows");
        break;
    }
    if (n_tested) {
        printf(" %d expressions of %d terminals", n_tested, n_terminals);
    } else {
        printf(" all %d-terminal expressions", n_terminals);
    }
    if (test_nvars || test_svars) {
        printf(" with");
        if (test_nvars) {
            printf(" %d numeric vars (each %d bits) in terms of operators",
                   test_nvars, test_bits);
            for (unsigned int relops = test_relops; relops;
                 relops = zero_rightmost_1bit(relops)) {
                enum expr_relop r = rightmost_1bit_idx(relops);
                printf(" %s", expr_relop_to_string(r));
            }
        }
        if (test_nvars && test_svars) {
            printf (" and");
        }
        if (test_svars) {
            printf(" %d string vars", test_svars);
        }
    } else {
        printf(" in terms of Boolean constants only");
    }
    printf(".\n");

    expr_symtab_destroy(&symtab);
    shash_destroy(&symtab);
}

/* Actions. */

static void
test_parse_actions(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct shash symtab;
    struct simap ports, ct_zones;
    struct ds input;

    create_symtab(&symtab);

    simap_init(&ports);
    simap_put(&ports, "eth0", 5);
    simap_put(&ports, "eth1", 6);
    simap_put(&ports, "LOCAL", ofp_to_u16(OFPP_LOCAL));
    simap_init(&ct_zones);

    ds_init(&input);
    while (!ds_get_test_line(&input, stdin)) {
        struct ofpbuf ofpacts;
        struct expr *prereqs;
        char *error;

        ofpbuf_init(&ofpacts, 0);
        error = actions_parse_string(ds_cstr(&input), &symtab, &ports,
                                     &ct_zones, 16, 16, 10, 64,
                                     &ofpacts, &prereqs);
        if (!error) {
            struct ds output;

            ds_init(&output);
            ds_put_cstr(&output, "actions=");
            ofpacts_format(ofpacts.data, ofpacts.size, &output);
            ds_put_cstr(&output, ", prereqs=");
            if (prereqs) {
                expr_format(prereqs, &output);
            } else {
                ds_put_char(&output, '1');
            }
            puts(ds_cstr(&output));
            ds_destroy(&output);
        } else {
            puts(error);
            free(error);
        }

        expr_destroy(prereqs);
        ofpbuf_uninit(&ofpacts);
    }
    ds_destroy(&input);

    simap_destroy(&ports);
    simap_destroy(&ct_zones);
    expr_symtab_destroy(&symtab);
    shash_destroy(&symtab);
}

static unsigned int
parse_relops(const char *s)
{
    unsigned int relops = 0;
    struct lexer lexer;

    lexer_init(&lexer, s);
    lexer_get(&lexer);
    do {
        enum expr_relop relop;

        if (expr_relop_from_token(lexer.token.type, &relop)) {
            relops |= 1u << relop;
            lexer_get(&lexer);
        } else {
            ovs_fatal(0, "%s: relational operator expected at `%.*s'",
                      s, (int) (lexer.input - lexer.start), lexer.start);
        }
        lexer_match(&lexer, LEX_T_COMMA);
    } while (lexer.token.type != LEX_T_END);
    lexer_destroy(&lexer);

    return relops;
}

static void
usage(void)
{
    printf("\
%s: OVN test utility\n\
usage: test-ovn %s [OPTIONS] COMMAND [ARG...]\n\
\n\
lex\n\
  Lexically analyzes OVN input from stdin and print them back on stdout.\n\
\n\
parse-expr\n\
annotate-expr\n\
simplify-expr\n\
normalize-expr\n\
expr-to-flows\n\
  Parses OVN expressions from stdin and print them back on stdout after\n\
  differing degrees of analysis.  Available fields are based on packet\n\
  headers.\n\
\n\
evaluate-expr A B C\n\
  Parses OVN expressions from stdin, evaluate them with assigned values,\n\
  and print the results on stdout.  Available fields are 'a', 'b', and 'c'\n\
  of 3 bits each.  A, B, and C should be in the range 0 to 7.\n\
\n\
composition N\n\
  Prints all the compositions of N on stdout.\n\
\n\
tree-shape N\n\
  Prints all the tree shapes with N terminals on stdout.\n\
\n\
exhaustive N\n\
  Tests that all possible Boolean expressions with N terminals are properly\n\
  simplified, normalized, and converted to flows.  Available options:\n\
   Overall options:\n\
    --operation=OPERATION  Operation to test, one of: convert, simplify,\n\
        normalize, flow.  Default: flow.  'normalize' includes 'simplify',\n\
        'flow' includes 'simplify' and 'normalize'.\n\
    --parallel=N  Number of processes to use in parallel, default 1.\n\
   Numeric vars:\n\
    --nvars=N  Number of numeric vars to test, in range 0...4, default 2.\n\
    --bits=N  Number of bits per variable, in range 1...3, default 3.\n\
    --relops=OPERATORS   Test only the specified Boolean operators.\n\
                         OPERATORS may include == != < <= > >=, space or\n\
                         comma separated.  Default is all operators.\n\
   String vars:\n\
    --svars=N  Number of string vars to test, in range 0...4, default 2.\n\
",
           program_name, program_name);
    exit(EXIT_SUCCESS);
}

static void
test_ovn_main(int argc, char *argv[])
{
    enum {
        OPT_RELOPS = UCHAR_MAX + 1,
        OPT_NVARS,
        OPT_SVARS,
        OPT_BITS,
        OPT_OPERATION,
        OPT_PARALLEL
    };
    static const struct option long_options[] = {
        {"relops", required_argument, NULL, OPT_RELOPS},
        {"nvars", required_argument, NULL, OPT_NVARS},
        {"svars", required_argument, NULL, OPT_SVARS},
        {"bits", required_argument, NULL, OPT_BITS},
        {"operation", required_argument, NULL, OPT_OPERATION},
        {"parallel", required_argument, NULL, OPT_PARALLEL},
        {"more", no_argument, NULL, 'm'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    char *short_options = ovs_cmdl_long_options_to_short_options(long_options);

    set_program_name(argv[0]);

    test_relops = parse_relops("== != < <= > >=");
    for (;;) {
        int option_index = 0;
        int c = getopt_long (argc, argv, short_options, long_options,
                             &option_index);

        if (c == -1) {
            break;
        }
        switch (c) {
        case OPT_RELOPS:
            test_relops = parse_relops(optarg);
            break;

        case OPT_NVARS:
            test_nvars = atoi(optarg);
            if (test_nvars < 0 || test_nvars > 4) {
                ovs_fatal(0, "number of numeric variables must be "
                          "between 0 and 4");
            }
            break;

        case OPT_SVARS:
            test_svars = atoi(optarg);
            if (test_svars < 0 || test_svars > 4) {
                ovs_fatal(0, "number of string variables must be "
                          "between 0 and 4");
            }
            break;

        case OPT_BITS:
            test_bits = atoi(optarg);
            if (test_bits < 1 || test_bits > 3) {
                ovs_fatal(0, "number of bits must be between 1 and 3");
            }
            break;

        case OPT_OPERATION:
            if (!strcmp(optarg, "convert")) {
                operation = OP_CONVERT;
            } else if (!strcmp(optarg, "simplify")) {
                operation = OP_SIMPLIFY;
            } else if (!strcmp(optarg, "normalize")) {
                operation = OP_NORMALIZE;
            } else if (!strcmp(optarg, "flow")) {
                operation = OP_FLOW;
            } else {
                ovs_fatal(0, "%s: unknown operation", optarg);
            }
            break;

        case OPT_PARALLEL:
            test_parallel = atoi(optarg);
            break;

        case 'm':
            verbosity++;
            break;

        case 'h':
            usage();

        case '?':
            exit(1);

        default:
            abort();
        }
    }
    free(short_options);

    static const struct ovs_cmdl_command commands[] = {
        /* Lexer. */
        {"lex", NULL, 0, 0, test_lex},

        /* Expressions. */
        {"parse-expr", NULL, 0, 0, test_parse_expr},
        {"annotate-expr", NULL, 0, 0, test_annotate_expr},
        {"simplify-expr", NULL, 0, 0, test_simplify_expr},
        {"normalize-expr", NULL, 0, 0, test_normalize_expr},
        {"expr-to-flows", NULL, 0, 0, test_expr_to_flows},
        {"evaluate-expr", NULL, 1, 1, test_evaluate_expr},
        {"composition", NULL, 1, 1, test_composition},
        {"tree-shape", NULL, 1, 1, test_tree_shape},
        {"exhaustive", NULL, 1, 1, test_exhaustive},

        /* Actions. */
        {"parse-actions", NULL, 0, 0, test_parse_actions},

        {NULL, NULL, 0, 0, NULL},
    };
    struct ovs_cmdl_context ctx;
    ctx.argc = argc - optind;
    ctx.argv = argv + optind;
    ovs_cmdl_run_command(&ctx, commands);
}

OVSTEST_REGISTER("test-ovn", test_ovn_main);
