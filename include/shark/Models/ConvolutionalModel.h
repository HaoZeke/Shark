/*!
 *
 *
 * \brief       Implements a model applying a convolution to an image
 *
 *
 *
 * \author    
 * \date        2017
 *
 *
 * \par Copyright 1995-2017 Shark Development Team
 *
 * <BR><HR>
 * This file is part of Shark.
 * <http://shark-ml.org/>
 *
 * Shark is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Shark is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Shark.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#ifndef SHARK_MODELS_CONV2DModel_H
#define SHARK_MODELS_CONV2DModel_H

#include <shark/Models/AbstractModel.h>
#include <shark/Models/NeuronLayers.h>
#include <shark/LinAlg/BLAS/kernels/conv2d.hpp>
#include <shark/Core/Images/Padding.h>
namespace shark {

///
/// \brief Convolutional Model for 2D image data.
///
/// \par
/// This model computes the result of
/// \f$ y = f(x) = g(\text{convolution}(w, x) + b) \f$, where g is an arbitrary activation function and
/// convolution is the convolution of the input image x with the filters w.  b is a vector with one entry for each filter which is then applied to each response above
///
/// The image is allowed to have several channels and are linearized to a single vector of size width * height * numChannels.
/// This is done by itnerleaving channels, i.e. for a pixel all channels are stored contiguously. Then the pixels are stored in
/// a row-major scheme.
///
/// For handling edge condition, the Conv2D model handles two different convolution modes:
///
/// Padding::Valid:
/// The output is only computed on patches which are fully inside the unpadded image as a linearized vector in the same format
/// of size (width - filter_width+1) * (height - filter_height+1) * numFilters.
///
/// Padding::ZeroPad
/// The output input is padded with zeros and the output has the same size as the input
/// of size width * height * numFilters.
template <class VectorType = RealVector, class ActivationFunction = LinearNeuron>
class Conv2DModel : public AbstractModel<VectorType,VectorType,VectorType>{
private:
	typedef AbstractModel<VectorType,VectorType, VectorType> base_type;
	typedef Conv2DModel<VectorType, ActivationFunction> self_type;

	static_assert(!std::is_same<typename VectorType::storage_type::storage_tag, blas::dense_tag>::value, "Conv2D not implemented for sparse inputs");
public:
	typedef typename base_type::BatchOutputType BatchOutputType;
	typedef typename base_type::BatchInputType BatchInputType;
	typedef typename base_type::ParameterVectorType ParameterVectorType;

	/// Default Constructor; use setStructure later.
	Conv2DModel(){
		base_type::m_features |= base_type::HAS_FIRST_PARAMETER_DERIVATIVE;
		base_type::m_features |= base_type::HAS_FIRST_INPUT_DERIVATIVE;
	}
	
	///\brief Sets the structure by setting the dimensionalities of image and filters.
	///
	/// \arg imageShape Shape of the image imHeight x imWidth x channel
	/// \arg filterShape Shape of the filter matrix numFilters x fiHeight x fiWidth x channel
	/// \arg type Type of convolution padding to perform
	Conv2DModel(
		Shape const& imageShape, Shape const& filterShape, Padding type = Padding::ZeroPad
	){
		base_type::m_features |= base_type::HAS_FIRST_PARAMETER_DERIVATIVE;
		base_type::m_features |= base_type::HAS_FIRST_INPUT_DERIVATIVE;
		setStructure(imageShape, filterShape, type);
	}

	std::string name() const
	{ return "Conv2DModel"; }
	
	///\brief Returns the expected shape of the input
	Shape inputShape() const{
		return {m_imageHeight, m_imageWidth, m_numChannels};
	}
	///\brief Returns the shape of the output
	Shape outputShape() const{
		if(m_type != Padding::Valid){
			return {m_imageHeight, m_imageWidth, m_numFilters};
		}else{
			return {m_imageHeight - m_filterHeight + 1, m_imageWidth - m_filterWidth + 1, m_numFilters};
		}
	}
	
	/// \brief Returns the activation function.
	ActivationFunction const& activationFunction()const{
		return m_activation;
	}
	
	/// \brief Returns the activation function.
	ActivationFunction& activationFunction(){
		return m_activation;
	}

	/// \brief Obtain the parameter vector.
	ParameterVectorType parameterVector() const{
		return m_filters | m_offset;
	}

	/// \brief Set the new parameters of the model.
	void setParameterVector(ParameterVectorType const& newParameters){
		SIZE_CHECK(newParameters.size() == numberOfParameters());
		noalias(m_filters) = subrange(newParameters,0,m_filters.size());
		noalias(m_offset) = subrange(newParameters,m_filters.size(),newParameters.size());
		updateBackpropFilters();
	}

	/// \brief Return the number of parameters.
	size_t numberOfParameters() const{
		return m_filters.size() + m_offset.size();
	}

	///\brief Sets the structure by setting the shape of image and filters.
	///
	/// \arg imageShape Shape of the image imHeight x imWidth x channel
	/// \arg filterShape Shape of the filter matrix numFilters x fiHeight x fiWidth
	/// \arg type Type of convolution padding to perform
	void setStructure(
		Shape const& imageShape, Shape const& filterShape, Padding type = Padding::ZeroPad
	){
		m_type = type;
		m_imageHeight = imageShape[0];
		m_imageWidth = imageShape[1];
		m_numChannels = imageShape[2];
		m_numFilters = filterShape[0];
		m_filterHeight = filterShape[1];
		m_filterWidth = filterShape[2];
		m_filters.resize(m_filterHeight * m_filterWidth * m_numFilters * m_numChannels);
		m_offset.resize(m_numFilters);
		updateBackpropFilters();
	}

	boost::shared_ptr<State> createState()const{
		return boost::shared_ptr<State>(new typename ActivationFunction::State());
	}

	using base_type::eval;

	/// Evaluate the model
	void eval(BatchInputType const& inputs, BatchOutputType& outputs, State& state)const{
		SIZE_CHECK(inputs.size2() == inputShape().numElements());
		outputs.resize(inputs.size1(),outputShape().numElements());
		outputs.clear();
		//geometry for "zero pad"
		std::size_t outputsForFilter = outputShape().numElements()/m_numFilters;
		std::size_t paddingHeight = (m_type != Padding::Valid) ? m_filterHeight - 1: 0;
		std::size_t paddingWidth = (m_type != Padding::Valid) ? m_filterWidth - 1: 0;
		
		blas::kernels::conv2d(inputs, m_filters, outputs,
			m_numChannels, m_numFilters, 
			m_imageHeight, m_imageWidth,
			m_filterHeight, m_filterWidth,
			paddingHeight, paddingWidth
		);
		//reshape matrix for offset
		auto reshapedOutputs = to_matrix(to_vector(outputs), outputsForFilter * inputs.size1(), m_numFilters);
		noalias(reshapedOutputs ) += blas::repeat(m_offset,outputsForFilter * inputs.size1());
		m_activation.evalInPlace(outputs, state.toState<typename ActivationFunction::State>());
	}

	///\brief Calculates the first derivative w.r.t the parameters and summing them up over all inputs of the last computed batch
	void weightedParameterDerivative(
		BatchInputType const& inputs,
		BatchOutputType const& outputs,
		BatchOutputType const& coefficients,
		State const& state,
		ParameterVectorType& gradient
	)const{
		SIZE_CHECK(coefficients.size2()==outputShape().numElements());
		SIZE_CHECK(coefficients.size1()==inputs.size1());
		std::size_t n = inputs.size1();
		BatchOutputType delta = coefficients;
		m_activation.multiplyDerivative(outputs,delta, state.toState<typename ActivationFunction::State>());
		
		gradient.resize(numberOfParameters());
		auto weightGradient = to_matrix(subrange(gradient,0,m_filters.size()), m_numFilters, m_filters.size()/m_numFilters);
		//~ auto weightGradient = subrange(gradient,0,m_filters.size());
		auto offsetGradient = subrange(gradient, m_filters.size(),gradient.size());
		
		//~ std::size_t paddingHeight = (m_type != Padding::Valid) ? m_filterHeight - 1: 0;
		//~ std::size_t paddingWidth = (m_type != Padding::Valid) ? m_filterWidth - 1: 0;
		
		//derivative of filters
		//~ BatchInputType responses(inputs.size1(), m_filters.size());
		//~ blas::kernels::conv2d(inputs, to_vector(delta), responses,
			//~ m_numChannels, m_numFilters, 
			//~ m_imageHeight, m_imageWidth,
			//~ m_imageHeight - paddingHeight, m_filterWidth - paddingWidth,
			//~ paddingHeight, paddingWidth
		//~ );
		//~ noalias(weightGradient) = sum_rows(responses);
		//derivatives of offset parameters
		//reshape coefficient matrix  into a matrix where the rows are the single output pixels
		//~ auto delta_pixels = to_matrix(to_vector(delta), coefficients.size1() * coefficients.size2()/m_numFilters, m_numFilters);
		//~ noalias(offsetGradient) = sum_rows(delta_pixels);
		
		BatchInputType patches(n * outputShape().numElements()/m_numFilters, m_filters.size()/m_numFilters);
		if(m_type == Padding::Valid){
			blas::bindings::im2mat(//todo must get public interface in remora
				inputs,patches,
				m_numChannels, 
				m_imageHeight, m_imageWidth,
				m_filterHeight, m_filterWidth
			);
		}else{
			blas::bindings::im2mat_pad(//todo must get public interface in remora
				inputs,patches,
				m_numChannels, 
				m_imageHeight, m_imageWidth,
				m_filterHeight, m_filterWidth,
				m_filterHeight - 1, m_filterWidth - 1
			);
		}
		auto delta_mat = to_matrix(to_vector(delta), patches.size1(), m_numFilters);
		noalias(weightGradient) = trans(delta_mat) % patches;
		noalias(offsetGradient) = sum_rows(delta_mat);
	}
	///\brief Calculates the first derivative w.r.t the inputs and summs them up over all inputs of the last computed batch
	void weightedInputDerivative(
		BatchInputType const & inputs,
		BatchOutputType const& outputs,
		BatchOutputType const & coefficients,
		State const& state,
		BatchInputType& derivatives
	)const{
		SIZE_CHECK(coefficients.size2() == outputShape().numElements());
		SIZE_CHECK(coefficients.size1() == inputs.size1());
		
		BatchOutputType delta = coefficients;
		m_activation.multiplyDerivative(outputs,delta, state.toState<typename ActivationFunction::State>());
		Shape shape = outputShape();
		std::size_t paddingHeight = m_filterHeight - 1;
		std::size_t paddingWidth = m_filterWidth - 1;
		if(m_type == Padding::Valid){
			paddingHeight *=2;
			paddingWidth *=2;
		}
		derivatives.resize(inputs.size1(),inputShape().numElements());
		derivatives.clear();
		blas::kernels::conv2d(delta, m_backpropFilters, derivatives,
			m_numFilters, m_numChannels, 
			shape[0], shape[1],
			m_filterHeight, m_filterWidth,
			paddingHeight, paddingWidth
		);
	}

	/// From ISerializable
	void read(InArchive& archive){
		archive >> m_filters;
		archive >> m_offset;
		archive >> m_imageHeight;
		archive >> m_imageWidth;
		archive >> m_filterHeight;
		archive >> m_filterWidth;
		archive >> m_numChannels;
		archive >> m_numFilters;
		archive >> m_type;
		updateBackpropFilters();
	}
	/// From ISerializable
	void write(OutArchive& archive) const{
		archive << m_filters;
		archive << m_offset;
		archive << m_imageHeight;
		archive << m_imageWidth;
		archive << m_filterHeight;
		archive << m_filterWidth;
		archive << m_numChannels;
		archive << m_numFilters;
		archive << m_type;
	}
	
private:
	
	///\brief Converts the filters into the backprop filters
	///
	/// for computing the derivatie wrt the inputs in the chain rule, we 
	/// have to convove the outer derivative with the "transposed" filters
	/// the transposition is done by switching the order of channels and filters in the storage
	void updateBackpropFilters(){
		m_backpropFilters.resize(m_filters.size());
		
		std::size_t filterImSize = m_filterWidth * m_filterHeight;
		std::size_t filterSize = m_numChannels * m_filterWidth * m_filterHeight;
		std::size_t bpFilterSize = m_numFilters * m_filterWidth * m_filterHeight;
		
		//Note: this looks a bit funny, but this way on a gpu only m_numChannel kernels need to be run
		for(std::size_t c = 0; c != m_numChannels; ++c){
			auto channel_mat = subrange(
				to_matrix(m_filters, m_numFilters, filterSize), //create matrix where each row is one filter
				0, m_numFilters, c * filterImSize, (c+1) * filterImSize //cut out all columns belonging to the current channel
			);
			//Todo: commented out, because we also need to flip, which is not implemented in remora
			//~ auto channel_vec = to_vector(flip(channel_mat));//flip and linearize to vector (flip not implemented)
			//~ //cut out target are and set values
			//~ noalias(subrange(m_backpropFilters, c * bpFilterSize, (c+1) * bpFilterSize)) = channel_vec;
			//instead use this cpu-only version
			auto target_vec = subrange(m_backpropFilters, c * bpFilterSize, (c+1) * bpFilterSize);
			auto target_mat = to_matrix(target_vec,m_numFilters, m_filterWidth * m_filterHeight);
			for(std::size_t f = 0; f != m_numFilters; ++f){
				for(std::size_t i = 0; i !=  m_filterWidth * m_filterHeight; ++i){
					target_mat(f,i) = channel_mat(f, m_filterWidth * m_filterHeight-i-1);
				}
			}
		}
	}
	VectorType m_filters; ///< Filters used for performing the convolution
	VectorType m_backpropFilters;///< Same as filter just with the storage order of filters and channels reversed.
	VectorType m_offset;///< offset applied to each filters response
	ActivationFunction m_activation;///< The activation function to use

	std::size_t m_imageHeight;
	std::size_t m_imageWidth;
	std::size_t m_filterHeight;
	std::size_t m_filterWidth;
	std::size_t m_numChannels;
	std::size_t m_numFilters;
	
	Padding m_type;
};


}
#endif
