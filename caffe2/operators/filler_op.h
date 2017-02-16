#ifndef CAFFE2_OPERATORS_FILLER_OP_H_
#define CAFFE2_OPERATORS_FILLER_OP_H_

#include "caffe2/core/context.h"
#include "caffe2/core/logging.h"
#include "caffe2/core/operator.h"
#include "caffe2/utils/math.h"

namespace caffe2 {

// FillerOp takes in either zero or one input.
//
// If the number of input is 1, the shape will be identical to that of the input
// at run time with optional additional dimensions appended at the end as
// specified by "extra_shape" argument. In that case the "shape" parameter
// should not be set.
//
// If the number of inputs is 0, the full shape must be provided via "shape"
// argument
template <class Context>
class FillerOp : public Operator<Context> {
 public:
  FillerOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator<Context>(operator_def, ws),
        shape_(ToVectorTIndex(OperatorBase::GetRepeatedArgument<int>("shape"))),
        extra_shape_(ToVectorTIndex(
            OperatorBase::GetRepeatedArgument<int>("extra_shape"))),
        input_as_shape_(
            OperatorBase::GetSingleArgument<bool>("input_as_shape", false)) {
    if (InputSize()) {
      if (shape_.size() != 0) {
        CAFFE_THROW(
            "Cannot set the shape argument and pass in an input at "
            "the same time");
      }
    } else {
      if (!extra_shape_.empty()) {
        CAFFE_THROW("Cannot set extra_shape when there is no input");
      }
      if (input_as_shape_) {
        CAFFE_THROW("An input must be given if input_as_shape is true");
      }
    }
  }

  virtual ~FillerOp() {}
  USE_OPERATOR_CONTEXT_FUNCTIONS;

  bool RunOnDevice() override {
    auto* output = Operator<Context>::Output(0);
    if (InputSize()) {
      auto& input = Input(0);
      auto shape = vector<TIndex>{};
      if (input_as_shape_) {
        CAFFE_ENFORCE_EQ(
            input.ndim(),
            1,
            "When input_as_shape is true, the input must be a 1D tensor of "
            "data type TIndex");
        auto* shape_data = input.template data<TIndex>();
        shape.insert(shape.end(), shape_data, shape_data + input.dim32(0));
      } else {
        shape.insert(shape.end(), input.dims().begin(), input.dims().end());
      }
      shape.insert(shape.end(), extra_shape_.begin(), extra_shape_.end());
      output->Resize(shape);
    } else {
      output->Resize(shape_);
    }
    return Fill(output);
  }

  virtual bool Fill(Tensor<Context>* output) = 0;

 protected:
  vector<TIndex> shape_;
  vector<TIndex> extra_shape_;
  bool input_as_shape_;
};

template <typename T, class Context>
class UniformFillOp final : public FillerOp<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  UniformFillOp(const OperatorDef& operator_def, Workspace* ws)
      : FillerOp<Context>(operator_def, ws),
        min_(OperatorBase::template GetSingleArgument<T>("min", 0)),
        max_(OperatorBase::template GetSingleArgument<T>("max", 1)) {
    DCHECK_LT(min_, max_) << "Max value should be bigger than min value.";
  }

  bool Fill(Tensor<Context>* output) override {
    math::RandUniform<T, Context>(
        output->size(),
        min_,
        max_,
        output->template mutable_data<T>(),
        &context_);
    return true;
  }

 private:
  T min_;
  T max_;
};

template <class Context>
class ConstantFillOp final : public FillerOp<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  ConstantFillOp(const OperatorDef& operator_def, Workspace* ws)
      : FillerOp<Context>(operator_def, ws) {
    TensorProto_DataType dtype =
        static_cast<TensorProto_DataType>(OperatorBase::GetSingleArgument<int>(
            "dtype", TensorProto_DataType_FLOAT));

    if (!OperatorBase::HasArgument("dtype") &&
        OperatorBase::HasArgument("value")) {
      // If 'dtype' is not provided, infer type based on the type of 'value'
      // Currently, single argument contains either float, int64 or bytes
      if (OperatorBase::HasSingleArgumentOfType<float>("value")) {
        dtype = TensorProto_DataType_FLOAT;
      } else if (OperatorBase::HasSingleArgumentOfType<int64_t>("value")) {
        dtype = TensorProto_DataType_INT64;
      } else {
        CAFFE_THROW("Argument 'value' is of unexpected type");
      }
      VLOG(1) << "Argument 'dtype' is not provided. Assume the data type is "
              << "the same as that of argument 'value': " << dtype;
    }

    switch (dtype) {
      case TensorProto_DataType_FLOAT:
        body_ = &ConstantFillOp::FillWithType<float>;
        break;
      case TensorProto_DataType_INT32:
        body_ = &ConstantFillOp::FillWithType<int>;
        break;
      case TensorProto_DataType_BOOL:
        body_ = &ConstantFillOp::FillWithType<bool>;
        break;
      case TensorProto_DataType_INT64:
        body_ = &ConstantFillOp::FillWithType<int64_t>;
        break;
      case TensorProto_DataType_UNDEFINED:
        CAFFE_THROW("ConstantFill op cannot have undefined 'dtype' argument");
      // break;
      default:
        CAFFE_THROW("Unexpected 'dtype' argument value: ", dtype);
    }
  }

  bool Fill(Tensor<Context>* output) override {
    return (this->*body_)(output);
  }

  template <typename T>
  bool FillWithType(Tensor<Context>* output) {
    T value = OperatorBase::GetSingleArgument<T>("value", 0);
    auto* data = output->template mutable_data<T>();
    if (output->size()) {
      math::Set<T, Context>(output->size(), value, data, &context_);
    }
    return true;
  }

 private:
  bool (ConstantFillOp::*body_)(Tensor<Context>* output);
};

template <typename T, class Context>
class GaussianFillOp final : public FillerOp<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  GaussianFillOp(const OperatorDef& operator_def, Workspace* ws)
      : FillerOp<Context>(operator_def, ws),
        mean_(OperatorBase::template GetSingleArgument<float>("mean", 0)),
        std_(OperatorBase::template GetSingleArgument<float>("std", 1)) {
    DCHECK_GT(std_, 0) << "Standard deviation should be nonnegative.";
  }

  bool Fill(Tensor<Context>* output) override {
    math::RandGaussian<T, Context>(
        output->size(),
        mean_,
        std_,
        output->template mutable_data<T>(),
        &context_);
    return true;
  }

 private:
  T mean_;
  T std_;
};

template <typename T, class Context>
class XavierFillOp final : public FillerOp<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  XavierFillOp(const OperatorDef& operator_def, Workspace* ws)
      : FillerOp<Context>(operator_def, ws) {}

  bool Fill(Tensor<Context>* output) override {
    const int fan_in = output->size() / output->dim32(0);
    T scale = std::sqrt(T(3) / fan_in);
    math::RandUniform<T, Context>(
        output->size(),
        -scale,
        scale,
        output->template mutable_data<T>(),
        &context_);
    return true;
  }
};

template <typename T, class Context>
class MSRAFillOp final : public FillerOp<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  MSRAFillOp(const OperatorDef& operator_def, Workspace* ws)
      : FillerOp<Context>(operator_def, ws) {}

  bool Fill(Tensor<Context>* output) override {
    const int fan_out = output->size() / output->dim32(1);
    T scale = std::sqrt(T(2) / fan_out);
    math::RandGaussian<T, Context>(
        output->size(),
        0.0,
        scale,
        output->template mutable_data<T>(),
        &context_);
    return true;
  }
};

// This is mostly used just as a debugging purpose stuff: it fills a tensor
// sequentially with values 0, 1, 2..., which can then be used to check e.g.
// reshape operations by allowing one to read the indices more easily.
template <typename T, class Context>
class RangeFillOp final : public FillerOp<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  RangeFillOp(const OperatorDef& operator_def, Workspace* ws)
      : FillerOp<Context>(operator_def, ws) {}

  bool Fill(Tensor<Context>* output) override;
};

template <class Context>
class LengthsRangeFillOp : public Operator<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  USE_SIMPLE_CTOR_DTOR(LengthsRangeFillOp);

  bool RunOnDevice() override {
    auto& input = Input(0);
    auto* output = Output(0);
    auto* input_data = input.template data<int32_t>();

    CAFFE_ENFORCE_EQ(input.ndim(), 1, "Input must be a vector.");

    auto len_sum = std::accumulate(input_data, input_data + input.size(), 0);

    output->Resize(len_sum);
    auto* output_data = output->template mutable_data<int32_t>();

    int32_t offset = 0;
    for (int i = 0; i < input.size(); ++i) {
      auto len = input_data[i];
      auto start = output_data + offset;
      std::iota(
          start,
          start + len,
          0); // make the third argument the arg of this operator
      offset += len;
    }
    return true;
  }
};

inline std::vector<TensorShape> FillerTensorInference(
    const OperatorDef& def,
    const vector<TensorShape>& in) {
  vector<TensorShape> out(1);
  ArgumentHelper helper(def);
  out[0].set_data_type(static_cast<TensorProto_DataType>(
      helper.GetSingleArgument<int>("dtype", TensorProto_DataType_FLOAT)));

  if (in.size()) {
    // TODO
    bool input_as_shape =
        helper.GetSingleArgument<bool>("input_as_shape", false);
    if (input_as_shape) {
      out[0].set_unknown_shape(true);
      return out;
    }
    for (int d : in[0].dims()) {
      out[0].add_dims(d);
    }
  } else {
    auto shape = helper.GetRepeatedArgument<int>("shape");
    for (int d : shape) {
      out[0].add_dims(d);
    }
  }
  return out;
}

} // namespace caffe2

#endif // CAFFE2_OPERATORS_FILLER_OP_H_
