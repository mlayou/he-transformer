//*****************************************************************************
// Copyright 2018-2019 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#include <algorithm>
#include <boost/asio.hpp>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "ngraph/log.hpp"
#include "seal/he_seal_client.hpp"
#include "seal/seal.h"
#include "seal/seal_util.hpp"
#include "tcp/tcp_client.hpp"
#include "tcp/tcp_message.hpp"

ngraph::he::HESealClient::HESealClient(const std::string& hostname,
                                       const size_t port,
                                       const size_t batch_size,
                                       const std::vector<float>& inputs,
                                       bool complex_packing)
    : m_batch_size{batch_size},
      m_is_done(false),
      m_inputs{inputs},
      m_complex_packing(complex_packing) {
  boost::asio::io_context io_context;
  tcp::resolver resolver(io_context);
  auto endpoints = resolver.resolve(hostname, std::to_string(port));

  auto client_callback = [this](const ngraph::he::TCPMessage& message) {
    return handle_message(message);
  };

  m_tcp_client = std::make_shared<ngraph::he::TCPClient>(io_context, endpoints,
                                                         client_callback);

  io_context.run();
}

void ngraph::he::HESealClient::set_seal_context() {
  m_context = seal::SEALContext::Create(m_encryption_params, true,
                                        seal::sec_level_type::none);

  print_seal_context(*m_context);

  m_keygen = std::make_shared<seal::KeyGenerator>(m_context);
  m_relin_keys = std::make_shared<seal::RelinKeys>(m_keygen->relin_keys());
  m_public_key = std::make_shared<seal::PublicKey>(m_keygen->public_key());
  m_secret_key = std::make_shared<seal::SecretKey>(m_keygen->secret_key());
  m_encryptor = std::make_shared<seal::Encryptor>(m_context, *m_public_key);
  m_decryptor = std::make_shared<seal::Decryptor>(m_context, *m_secret_key);

  // Evaluator
  m_evaluator = std::make_shared<seal::Evaluator>(m_context);

  // Encoder
  m_ckks_encoder = std::make_shared<seal::CKKSEncoder>(m_context);

  // TODO: pick better scale?
  m_scale = ngraph::he::choose_scale(m_encryption_params.coeff_modulus());
  NGRAPH_INFO << "Client scale " << m_scale;
}

void ngraph::he::HESealClient::handle_message(
    const ngraph::he::TCPMessage& message) {
  ngraph::he::MessageType msg_type = message.message_type();

  NGRAPH_DEBUG << "Client received message type: "
               << message_type_to_string(msg_type).c_str();

  switch (msg_type) {
    case ngraph::he::MessageType::parameter_size: {
      // Number of (packed) ciphertexts to perform inference on
      size_t parameter_size;
      std::memcpy(&parameter_size, message.data_ptr(), message.data_size());

      NGRAPH_INFO << "Parameter size " << parameter_size;
      NGRAPH_INFO << "Client batch size " << m_batch_size;
      if (complex_packing()) {
        NGRAPH_INFO << "Client complex packing";
      }

      if (m_inputs.size() > parameter_size * m_batch_size) {
        NGRAPH_INFO << "m_inputs.size() " << m_inputs.size()
                    << " > paramter_size ( " << parameter_size
                    << ") * m_batch_size (" << m_batch_size << ")";
      }

      std::vector<seal::Ciphertext> ciphers(parameter_size);
#pragma omp parallel for
      for (size_t data_idx = 0; data_idx < parameter_size; ++data_idx) {
        seal::Plaintext plain;

        size_t batch_start_idx = data_idx * m_batch_size;
        size_t batch_end_idx = batch_start_idx + m_batch_size;

        std::vector<double> real_vals{m_inputs.begin() + batch_start_idx,
                                      m_inputs.begin() + batch_end_idx};
        if (complex_packing()) {
          std::vector<std::complex<double>> complex_vals;
          real_vec_to_complex_vec(complex_vals, real_vals);
          m_ckks_encoder->encode(complex_vals, m_scale, plain);
        } else {
          m_ckks_encoder->encode(real_vals, m_scale, plain);
        }
        m_encryptor->encrypt(plain, ciphers[data_idx]);
      }
      NGRAPH_INFO << "Creating execute message";
      auto execute_message =
          TCPMessage(ngraph::he::MessageType::execute, ciphers);
      NGRAPH_INFO << "Sending execute message with " << parameter_size
                  << " ciphertexts";
      write_message(std::move(execute_message));
      break;
    }
    case ngraph::he::MessageType::result: {
      size_t result_count = message.count();
      size_t element_size = message.element_size();

      std::vector<seal::Ciphertext> result;
      m_results.reserve(result_count * m_batch_size);
      for (size_t result_idx = 0; result_idx < result_count; ++result_idx) {
        seal::Ciphertext cipher;
        std::stringstream cipher_stream;
        cipher_stream.write(message.data_ptr() + result_idx * element_size,
                            element_size);
        cipher.load(m_context, cipher_stream);

        result.push_back(cipher);
        seal::Plaintext plain;
        m_decryptor->decrypt(cipher, plain);

        std::vector<double> outputs;
        decode_to_real_vec(plain, outputs, complex_packing());
        NGRAPH_CHECK(outputs.size() >= m_batch_size, "outputs.size() ",
                     outputs.size(), " < m_batch_size ", m_batch_size);
        m_results.insert(m_results.end(), outputs.begin(),
                         outputs.begin() + m_batch_size);
      }
      close_connection();
      break;
    }

    case ngraph::he::MessageType::none: {
      close_connection();
      break;
    }

    case ngraph::he::MessageType::encryption_parameters: {
      std::stringstream param_stream;
      param_stream.write(message.data_ptr(), message.element_size());
      m_encryption_params = seal::EncryptionParameters::Load(param_stream);
      NGRAPH_INFO << "Loaded encryption parmeters";

      set_seal_context();

      // Send public key
      std::stringstream pk_stream;
      m_public_key->save(pk_stream);
      auto pk_message = TCPMessage(ngraph::he::MessageType::public_key, 1,
                                   std::move(pk_stream));
      NGRAPH_INFO << "Sending public key";
      write_message(std::move(pk_message));

      // Send evaluation key
      std::stringstream evk_stream;
      m_relin_keys->save(evk_stream);
      auto evk_message = TCPMessage(ngraph::he::MessageType::eval_key, 1,
                                    std::move(evk_stream));
      NGRAPH_INFO << "Sending evaluation key";
      write_message(std::move(evk_message));

      break;
    }
    case ngraph::he::MessageType::relu6_request: {
      handle_relu_request(message);
      break;
    }
    case ngraph::he::MessageType::relu_request: {
      handle_relu_request(message);
      break;
    }

    case ngraph::he::MessageType::max_request: {
      size_t complex_pack_factor = complex_packing() ? 2 : 1;
      size_t cipher_count = message.count();
      size_t element_size = message.element_size();

      std::vector<std::vector<double>> input_cipher_values(
          m_batch_size * complex_pack_factor,
          std::vector<double>(cipher_count, 0));

      std::vector<double> max_values(m_batch_size * complex_pack_factor,
                                     std::numeric_limits<double>::lowest());

#pragma omp parallel for
      for (size_t cipher_idx = 0; cipher_idx < cipher_count; ++cipher_idx) {
        seal::Ciphertext pre_sort_cipher;
        seal::Plaintext pre_sort_plain;

        // Load cipher from stream
        std::stringstream pre_sort_cipher_stream;
        pre_sort_cipher_stream.write(
            message.data_ptr() + cipher_idx * element_size, element_size);
        pre_sort_cipher.load(m_context, pre_sort_cipher_stream);

        // Decrypt cipher
        m_decryptor->decrypt(pre_sort_cipher, pre_sort_plain);
        std::vector<double> pre_max_value;
        decode_to_real_vec(pre_sort_plain, pre_max_value, complex_packing());

        for (size_t batch_idx = 0;
             batch_idx < m_batch_size * complex_pack_factor; ++batch_idx) {
          input_cipher_values[batch_idx][cipher_idx] = pre_max_value[batch_idx];
        }
      }

      // Get max of each vector of values
      for (size_t batch_idx = 0; batch_idx < m_batch_size * complex_pack_factor;
           ++batch_idx) {
        max_values[batch_idx] =
            *std::max_element(input_cipher_values[batch_idx].begin(),
                              input_cipher_values[batch_idx].end());
      }

      // Encrypt maximum values
      seal::Ciphertext cipher_max;
      seal::Plaintext plain_max;
      std::stringstream max_stream;

      if (complex_packing()) {
        assert(max_values.size() % 2 == 0);
        std::vector<std::complex<double>> max_complex_vals;
        real_vec_to_complex_vec(max_complex_vals, max_values);
        m_ckks_encoder->encode(max_complex_vals, m_scale, plain_max);
      } else {
        m_ckks_encoder->encode(max_values, m_scale, plain_max);
      }
      m_encryptor->encrypt(plain_max, cipher_max);
      cipher_max.save(max_stream);

      auto max_result_msg = TCPMessage(ngraph::he::MessageType::max_result, 1,
                                       std::move(max_stream));
      write_message(std::move(max_result_msg));

      break;
    }
    case ngraph::he::MessageType::execute:
    case ngraph::he::MessageType::eval_key:
    case ngraph::he::MessageType::max_result:
    case ngraph::he::MessageType::minimum_request:
    case ngraph::he::MessageType::minimum_result:
    case ngraph::he::MessageType::parameter_shape_request:
    case ngraph::he::MessageType::public_key:
    case ngraph::he::MessageType::relu_result:
    case ngraph::he::MessageType::result_request:
    default:
      NGRAPH_INFO << "Unsupported message type: "
                  << message_type_to_string(msg_type).c_str();
  }
}

void ngraph::he::HESealClient::close_connection() {
  NGRAPH_INFO << "Closing connection";
  m_tcp_client->close();
  m_is_done = true;
}

void ngraph::he::HESealClient::handle_relu_request(
    const ngraph::he::TCPMessage& message) {
  auto relu = [=](double d) { return d > 0 ? d : 0; };
  auto relu6 = [=](double d) { return d > 6.0 ? 6.0 : (d > 0) ? d : 0.; };

  std::function<double(double)> activation;

  if (message.message_type() == ngraph::he::MessageType::relu6_request) {
    activation = relu6;
  } else if (message.message_type() == ngraph::he::MessageType::relu_request) {
    activation = relu;
  } else {
    throw ngraph_error("Non-relu message type in handle_relu_request");
  }

  size_t result_count = message.count();
  size_t element_size = message.element_size();
  NGRAPH_INFO << "Received Relu request with " << result_count << " elements"
              << " of size " << element_size;

  std::vector<seal::Ciphertext> post_relu_ciphers(result_count);
#pragma omp parallel for
  for (size_t result_idx = 0; result_idx < result_count; ++result_idx) {
    seal::Ciphertext pre_relu_cipher;
    seal::Plaintext relu_plain;

    // Load cipher from stream
    std::stringstream pre_relu_cipher_stream;
    pre_relu_cipher_stream.write(message.data_ptr() + result_idx * element_size,
                                 element_size);
    pre_relu_cipher.load(m_context, pre_relu_cipher_stream);

    // Decrypt cipher
    m_decryptor->decrypt(pre_relu_cipher, relu_plain);

    std::vector<double> relu_vals;
    decode_to_real_vec(relu_plain, relu_vals, complex_packing());
    std::vector<double> post_relu_vals(relu_vals.size());
    std::transform(relu_vals.begin(), relu_vals.end(), post_relu_vals.begin(),
                   activation);

    if (complex_packing()) {
      std::vector<std::complex<double>> complex_relu_vals;
      real_vec_to_complex_vec(complex_relu_vals, post_relu_vals);
      m_ckks_encoder->encode(complex_relu_vals, m_scale, relu_plain);
    } else {
      m_ckks_encoder->encode(post_relu_vals, m_scale, relu_plain);
    }
    m_encryptor->encrypt(relu_plain, post_relu_ciphers[result_idx]);
  }
  auto relu_result_msg =
      TCPMessage(ngraph::he::MessageType::relu_result, post_relu_ciphers);

  write_message(std::move(relu_result_msg));
  return;
}

void ngraph::he::HESealClient::decode_to_real_vec(const seal::Plaintext& plain,
                                                  std::vector<double>& output,
                                                  bool complex) {
  NGRAPH_CHECK(output.size() == 0);
  if (complex) {
    std::vector<std::complex<double>> complex_outputs;
    m_ckks_encoder->decode(plain, complex_outputs);
    complex_vec_to_real_vec(output, complex_outputs);
  } else {
    m_ckks_encoder->decode(plain, output);
    NGRAPH_CHECK(m_batch_size <= output.size());
    output.resize(m_batch_size);
  }
}
