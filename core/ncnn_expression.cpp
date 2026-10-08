/* The pinned model uses integer reshape dimensions, never expressions.
 * Reject expression evaluation instead of pulling scanf into SceShell. */
#include <expression.h>
namespace ncnn {
int count_expression_blobs(const std::string &) { return 0; }
int eval_list_expression(const std::string &, const std::vector<Mat> &, std::vector<int> &) { return -1; }
}
